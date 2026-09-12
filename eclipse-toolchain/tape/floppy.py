"""floppy.py -- wrap a compiled Eclipse program (or an already-assembled
DG absolute-binary .ab file) into a raw sector image (.img) matching the
real DG Model 6030 flexible-diskette subsystem's geometry, bootable via
the 6030's own Program Load hardware convention.

Sibling to tape/mktape.py: reuses that module's parse_ab()/flatten()/
write_ab()/parse_addr() wholesale (see the imports below) rather than
reimplementing DG absolute-binary parsing -- this module only adds the
floppy-specific geometry, boot mechanism, and .img packing/unpacking on
top of it. eclipse-ab2floppy and eclipse-floppy2ab are the driver
scripts, following eclipse-ab2tape/eclipse-tape2ab's own conventions.

Real geometry (manual-verified -- DG "Model 6030 Diskette Subsystem
Technical Reference", DGC 014-000065-03, Rev. 03, page I-6/I-7's
"PROGRAMMER'S REFERENCE INFORMATION" table, confirmed against the actual
page image, not OCR/paraphrase):
    TRACKS/SURFACE (CYLINDERS)  77
    SECTORS/TRACK                8
    WORDS/SECTOR                256   (512 bytes)
    SURFACES                     1    (single-sided: "While the subsystem
                                       controller may select heads 0-4,
                                       the diskette unit can only respond
                                       to head 0 selection" -- same page)
Independently corroborated (not just OCR-checked) by SIMH's own
NOVA/nova_dkp.c, whose TYPE_FLP ("6030 (floppy)") drive-type table entry
is SECT_FLP=8, SURF_FLP=1, CYL_FLP=77, DKP_NUMWD=256 words/sector --
exact numeric agreement with the manual, from an independent source.
This also fixes the on-disk word order: nova_dkp.c's dkp_svc() does a
straight fxread()/fxwrite() of native uint16 words with no byte-swap
(unlike the tape/MTA path, which DMA-packs bytes big-endian regardless
of host order -- see mktape.py's header comment) -- a floppy .img is
native-byte-order words throughout, exactly like a .ab file already is.
Total image size: 77 * 8 * 256 words * 2 bytes = 315392 bytes.

Boot convention (manual-verified for track 0 only -- see
boot_stage2_floppy.s.in's header comment for the full citation trail and
the honest real-hardware caveat this carries that the tape path doesn't):
Program Load hardware auto-DMAs all 8 sectors of track 0 (2048 words) to
memory address 0, then hands control to address 0377 (octal) via the
same "spin on a self-referential JMP until an in-flight DMA overwrites
that exact word" trap tape/boot_stage2.s.in already relies on and
documents. track 0 sector 0 (words 0-0377) holds this module's own
stage2 relocator (boot_stage2_floppy.s.in, assembled per-entry/per-size
just like tape's stage2); sectors 1-7 (words 0400-3777 octal, 1792
words) hold the program's raw words verbatim, landing at a fixed
hardware address (0400) that stage2 then relocates to the program's real
entry address. This is a project-authored mechanism (like tape's stage2
is), verified only by assembling with the real dgasm and booting/
single-stepping in this project's own SIMH ECLIPSE build (see
FLOPPY_NOTES.md) -- not against real 6030 hardware.

Known limit this module enforces (see boot_stage2_floppy.s.in): a
program must fit in 1792 words (3584 bytes) -- everything Program Load's
own hardware auto-loads, minus stage2's 256-word sector-0 budget. A
bigger program needs a stage2 that drives the controller itself to read
track 1 onward, which is real disk-driver work this module does not
attempt (see FLOPPY_NOTES.md).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mktape  # noqa: E402  -- reused: parse_ab, flatten, write_ab, parse_addr,
                # le_words_from_file, STAGE2_TEMPLATE-style plumbing

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
STAGE2_FLOPPY_TEMPLATE = os.path.join(SCRIPT_DIR, "boot_stage2_floppy.s.in")

# Real 6030 geometry -- see this module's header comment for citations.
CYLS = 77
SECTORS_PER_TRACK = 8
WORDS_PER_SECTOR = 256          # DKP_NUMWD in nova_dkp.c; 512 bytes
SURFACES = 1                    # single-sided

TRACK_WORDS = SECTORS_PER_TRACK * WORDS_PER_SECTOR         # 2048
TOTAL_WORDS = CYLS * SURFACES * TRACK_WORDS                # 157696
TOTAL_BYTES = TOTAL_WORDS * 2                               # 315392

# Track 0 layout (see boot_stage2_floppy.s.in's header comment):
#   sector 0  (words 0-0377 octal, 256 words):    stage2 relocator image
#   sectors 1-7 (words 0400-3777 octal, 1792 words): program's raw words,
#     landing at fixed address 0400 by Program Load hardware; stage2
#     relocates them to the program's real entry address.
STAGE2_SECTOR_WORDS = WORDS_PER_SECTOR                      # 256
LANDING_ZONE_ADDR = WORDS_PER_SECTOR                         # 0400 octal = 256
LANDING_ZONE_WORDS = TRACK_WORDS - STAGE2_SECTOR_WORDS       # 1792
MAX_PROGRAM_WORDS = LANDING_ZONE_WORDS


def words_to_bytes(words):
    """Native byte order (see this module's header comment: floppy .img
    words are read/written by nova_dkp.c with a plain fxread/fxwrite of
    uint16 -- no DMA byte-swap the way tape's MTA path has)."""
    return b"".join(struct.pack("<H", w & 0xFFFF) for w in words)


def bytes_to_words(data):
    if len(data) % 2:
        raise ValueError(f"odd byte count ({len(data)}), not a whole number of words")
    return [struct.unpack_from("<H", data, i)[0] for i in range(0, len(data), 2)]


def assemble_stage2_floppy(entry_addr, nwords, dgasm_cmd="dgasm"):
    """Substitute @ENTRY@ and @NWORDS@ in boot_stage2_floppy.s.in and
    assemble it with dgasm -f bin, returning the resulting words (index 0
    = content of address 1 -- dgasm's -f bin output for a file starting
    'org 1' begins at address 1, no leading pad for address 0; see
    mktape.py's assemble_stage2() for the identical convention)."""
    template = open(STAGE2_FLOPPY_TEMPLATE).read()
    filled = template.replace("@ENTRY@", f"0{entry_addr:o}").replace("@NWORDS@", f"0{nwords:o}")
    if ("@ENTRY@" in template or "@NWORDS@" in template) and (
        "@ENTRY@" in filled or "@NWORDS@" in filled
    ):
        raise AssertionError("boot_stage2_floppy.s.in has unsubstituted placeholders")

    import shutil
    import subprocess
    import tempfile

    work = tempfile.mkdtemp(prefix="eclipse-mkfloppy-")
    try:
        src = os.path.join(work, "boot_stage2_floppy.s")
        out = os.path.join(work, "boot_stage2_floppy.bin")
        with open(src, "w") as f:
            f.write(filled)
        result = subprocess.run(
            [dgasm_cmd, "-t", "eclipse_s140", "-f", "bin", "-o", out, src],
            capture_output=True, text=True,
        )
        if result.returncode != 0 or not os.path.exists(out):
            raise RuntimeError(
                f"dgasm failed assembling boot_stage2_floppy.s.in "
                f"(entry={entry_addr:#o}, nwords={nwords:#o}):\n"
                f"{result.stdout}{result.stderr}"
            )
        return mktape.le_words_from_file(out)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _stage2_sector0(entry_addr, nwords, dgasm_cmd):
    """Build the 256-word sector 0 image: [0 (dummy, address 0)] +
    stage2's own assembled body (addresses 1-0377), zero-padded -- same
    shape as mktape.py's build_tape() record 1, and the same "trailing
    word past the last org must be zero" sanity check."""
    stage2_words = assemble_stage2_floppy(entry_addr, nwords, dgasm_cmd)
    body, tail = stage2_words[:255], stage2_words[255:]
    if any(tail):
        raise ValueError(
            f"boot_stage2_floppy.s.in assembled to {len(stage2_words)} words with "
            f"nonzero content past address 0377 ({tail}) -- exceeds the 255 words "
            f"available before the boot ROM's trap address"
        )
    sector0 = [0] + body + [0] * (255 - len(body))
    assert len(sector0) == STAGE2_SECTOR_WORDS
    return sector0


def build_floppy(entry_addr, program_words, out_path, dgasm_cmd="dgasm"):
    """entry_addr: octal/decimal int, where the program really runs from
    (stage2 relocates it there). program_words: the program's flat
    memory image starting at entry_addr (no gaps -- caller fills them;
    see mktape.flatten()). Writes a full TOTAL_BYTES-byte .img matching
    real 6030 geometry: track 0 = stage2 + program (see this module's
    header comment), tracks 1-76 = zero (unused -- see MAX_PROGRAM_WORDS).
    Returns (sector0_len, program_len, total_bytes)."""
    if not program_words:
        raise ValueError("program is empty -- nothing to boot")
    if len(program_words) > MAX_PROGRAM_WORDS:
        raise ValueError(
            f"program is {len(program_words)} words, exceeds the "
            f"{MAX_PROGRAM_WORDS}-word single-track relocate budget (see "
            f"boot_stage2_floppy.s.in's 'Known limit' comment) -- a program "
            f"this size needs a stage2 that drives the 6030 controller itself "
            f"to read track 1 onward, which this tool does not implement"
        )

    sector0 = _stage2_sector0(entry_addr, len(program_words), dgasm_cmd)
    track0 = sector0 + list(program_words) + [0] * (LANDING_ZONE_WORDS - len(program_words))
    assert len(track0) == TRACK_WORDS

    image = track0 + [0] * (TOTAL_WORDS - TRACK_WORDS)
    assert len(image) == TOTAL_WORDS

    with open(out_path, "wb") as f:
        f.write(words_to_bytes(image))

    return len(sector0) * 2, len(program_words) * 2, TOTAL_BYTES


def floppy2ab(img_path, out_ab_path, dgasm_cmd="dgasm", verify=True):
    """Inverse of build_floppy(): recover (entry, program_words) from a
    .img this module built, and write them out as a dgasm -f ab file
    (reusing mktape.write_ab() directly). Verification (default on, same
    discipline as eclipse-tape2ab): reassembles boot_stage2_floppy.s.in
    for the recovered (entry, nwords) and diffs the result against the
    image's actual sector 0, refusing to guess on mismatch -- catches an
    .img that wasn't built by this tool, or was built by a
    build_floppy()-compatible tool but for different (entry, nwords) than
    sector 0 actually reflects. Returns (entry, nwords)."""
    data = open(img_path, "rb").read()
    if len(data) != TOTAL_BYTES:
        raise ValueError(
            f"{img_path}: {len(data)} bytes, expected exactly {TOTAL_BYTES} "
            f"({CYLS} cyls x {SECTORS_PER_TRACK} sectors x {WORDS_PER_SECTOR} "
            f"words x 2 bytes) -- not a 6030 image this toolchain built"
        )
    words = bytes_to_words(data)
    sector0 = words[0:STAGE2_SECTOR_WORDS]
    entry = sector0[1]
    nwords = sector0[2]

    if nwords <= 0 or nwords > LANDING_ZONE_WORDS:
        raise ValueError(
            f"{img_path}: recovered word count {nwords:#o} is out of range "
            f"(1-{LANDING_ZONE_WORDS:#o}) -- not an image this toolchain built, "
            f"or sector 0 is corrupt"
        )

    if verify:
        expected_sector0 = _stage2_sector0(entry, nwords, dgasm_cmd)
        if expected_sector0 != sector0:
            raise RuntimeError(
                f"{img_path}: track 0 sector 0 doesn't match "
                f"boot_stage2_floppy.s.in reassembled for entry={entry:#o}, "
                f"nwords={nwords:#o} -- this image wasn't built by this "
                f"toolchain for those values, refusing to guess (pass "
                f"verify=False / --no-verify to skip this check)"
            )

    program_words = words[LANDING_ZONE_ADDR:LANDING_ZONE_ADDR + nwords]
    mktape.write_ab(entry, program_words, out_ab_path)
    return entry, nwords
