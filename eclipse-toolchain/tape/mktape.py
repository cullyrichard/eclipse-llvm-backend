#!/usr/bin/env python3
"""mktape.py -- wrap a compiled Eclipse program into a bootable 9-track
tape image (SIMH .tap format) for eclipseemu's MTA device (and, in
principle, a real Eclipse's magtape controller -- see eclipse-mktape's
and eclipse-ab2tape's header comments for the same real-hardware caveat
eclipse-compile.sh's -f ab path carries: this has only been verified
against eclipseemu, not real tape hardware).

Used two ways:
  eclipse-mktape     compiles C source, then calls build_tape() here with
                      the resulting flat binary and its fixed entry (050,
                      every program this toolchain's own C pipeline
                      produces starts there).
  eclipse-ab2tape     packages an already-assembled .ab file (dgasm -f ab,
                      the DG object/loader block format) directly, no
                      compilation -- parses it with parse_ab() below and
                      passes its own entry address through.

Tape layout (see tape/boot_stage2.s.in's header comment for the full
mechanism this implements):
  record 1: exactly 256 big-endian words (addresses 0-0377 octal). Word 0
            is a dummy (address 0 is never used by anything that runs
            here); words 1-0377 are boot_stage2.s.in's own assembled
            image (entry address substituted in), zero-padded if
            shorter. Word 0377 -- wherever eclipseemu's (and real
            Eclipse hardware's) generic magtape boot ROM ends up
            trapping once this record's DMA overwrites it -- holds
            stage2's own entry jump.
  record 2: the program's raw memory image, big-endian words, to be
            DMA'd starting at its own entry address.
  tape mark: end of medium.

SIMH tape image format (documented in simh's NOVA/nova_mta.c and
sim_tape.c): each record is a 32-bit little-endian byte count, the
record's bytes (padded to even length), then the same 32-bit byte count
repeated; a tape mark is a bare 4-byte zero count with no data/repeat.

Byte order note: dgasm's own output formats (-f bin, -f ab) write 16-bit
words in the host's native byte order (little-endian on any x86/ARM
machine this toolchain is built on). The magtape controller's DMA path
(nova_mta.c's CU_READ: `M[pa] = (c1<<8)|c2`) instead pairs up
consecutive tape bytes big-endian (first byte -> high byte of the word).
Every word this script writes is therefore explicitly re-packed
big-endian, regardless of what byte order it was read in as.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile


def parse_addr(s):
    """Accept 050 (old-style octal, like every literal address in this
    codebase and its comments), 0o50/0x28/40 (Python's own int(x, 0)
    forms), all as equivalent -- int(x, 0) alone rejects a bare leading
    zero without "o"/"x" (Python 3 requires "0o50", not "050")."""
    if s.lower().startswith("0x") or s.lower().startswith("0o") or s.lower().startswith("0b"):
        return int(s, 0)
    if s.startswith("0") and len(s) > 1:
        return int(s, 8)
    return int(s, 10)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
STAGE2_TEMPLATE = os.path.join(SCRIPT_DIR, "boot_stage2.s.in")
MTA_MAX_RECORD_WORDS = 32768  # nova_mta.c: MTA_MAXFR = 1<<16 bytes


def write_tap(path, records):
    with open(path, "wb") as f:
        for rec in records:
            if rec is None:  # tape mark
                f.write(struct.pack("<I", 0))
                continue
            n = len(rec)
            f.write(struct.pack("<I", n))
            f.write(rec)
            if n & 1:
                f.write(b"\x00")
            f.write(struct.pack("<I", n))


def le_words_from_file(path):
    raw = open(path, "rb").read()
    if len(raw) % 2:
        raise ValueError(f"{path}: odd byte count ({len(raw)}), not a whole number of words")
    return [struct.unpack_from("<H", raw, i)[0] for i in range(0, len(raw), 2)]


def big_endian_bytes(words):
    return b"".join(struct.pack(">H", w & 0xFFFF) for w in words)


def parse_ab(path):
    """Parse a dgasm -f ab file (see output.c's write_absolute_binary):
    a sequence of blocks, each [int16 -blocksize][uint16 addr]
    [uint16 checksum][blocksize data words], native byte order, with no
    end-of-data trailer. Returns {address: word}."""
    data = open(path, "rb").read()
    words = {}
    i = 0
    while i < len(data):
        if i + 6 > len(data):
            raise ValueError(f"{path}: truncated block header at byte {i}")
        block_size, addr, _checksum = struct.unpack_from("<hHH", data, i)
        i += 6
        n = -block_size
        if n <= 0:
            raise ValueError(f"{path}: non-negative block size {block_size} at byte {i - 6}")
        for k in range(n):
            if i + 2 > len(data):
                raise ValueError(f"{path}: truncated block data at byte {i}")
            (w,) = struct.unpack_from("<H", data, i)
            words[addr + k] = w
            i += 2
    if not words:
        raise ValueError(f"{path}: no blocks found")
    return words


def flatten(words_by_addr):
    """{address: word} -> (start_addr, [word, word, ...]), gaps zero-filled."""
    lo, hi = min(words_by_addr), max(words_by_addr)
    return lo, [words_by_addr.get(a, 0) for a in range(lo, hi + 1)]


def assemble_stage2(entry_addr, dgasm_cmd="dgasm"):
    """Substitute @ENTRY@ in boot_stage2.s.in with entry_addr (octal) and
    assemble it with dgasm -f bin, returning the resulting words."""
    template = open(STAGE2_TEMPLATE).read()
    # dgasm's lexer only treats a numeral as octal if it has a leading
    # "0" (strtol(..., 0)); a bare "50" would parse as decimal 50, not
    # octal 050.
    filled = template.replace("@ENTRY@", f"0{entry_addr:o}")
    if "@ENTRY@" in template and filled == template:
        raise AssertionError("boot_stage2.s.in has no @ENTRY@ placeholder to substitute")

    work = tempfile.mkdtemp(prefix="eclipse-mktape-")
    try:
        src = os.path.join(work, "boot_stage2.s")
        out = os.path.join(work, "boot_stage2.bin")
        with open(src, "w") as f:
            f.write(filled)
        result = subprocess.run(
            [dgasm_cmd, "-t", "eclipse_s140", "-f", "bin", "-o", out, src],
            capture_output=True, text=True,
        )
        if result.returncode != 0 or not os.path.exists(out):
            raise RuntimeError(
                f"dgasm failed assembling boot_stage2.s.in (entry={entry_addr:#o}):\n"
                f"{result.stdout}{result.stderr}"
            )
        return le_words_from_file(out)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def build_tape(entry_addr, program_words, out_path, dgasm_cmd="dgasm"):
    """entry_addr: octal/decimal int, where record 2 is DMA'd to and
    where stage2 jumps once it's loaded. program_words: the program's
    flat memory image starting at entry_addr (no gaps -- caller fills
    them; see flatten())."""
    stage2_words = assemble_stage2(entry_addr, dgasm_cmd)
    # boot_stage2.s.in's own trailing `org 0377 / JMP @loadaddr` produces
    # one word *at* address 0377 (the last of the 255 words available:
    # 1-0377) plus dgasm -f bin's output span running one word past its
    # last written address (address 0400) -- that trailing word is a
    # fixed, always-zero artifact of the span calculation, not real
    # content, and is dropped here; anything beyond that genuinely means
    # boot_stage2.s.in grew past the address range this design has to
    # fit in.
    body, tail = stage2_words[:255], stage2_words[255:]
    if any(tail):
        raise ValueError(
            f"boot_stage2.s.in assembled to {len(stage2_words)} words with "
            f"nonzero content past address 0377 ({tail}) -- exceeds the "
            f"255 words available before the boot ROM's trap address"
        )
    record1_words = [0] + body + [0] * (255 - len(body))
    assert len(record1_words) == 256
    record1 = big_endian_bytes(record1_words)

    if len(program_words) > MTA_MAX_RECORD_WORDS:
        raise ValueError(
            f"program is {len(program_words)} words, exceeds the "
            f"{MTA_MAX_RECORD_WORDS}-word single-record limit (see "
            f"boot_stage2.s.in's 'Known limit' comment)"
        )
    record2 = big_endian_bytes(program_words)

    write_tap(out_path, [record1, record2, None])
    return len(record1), len(record2)


def _main():
    p = argparse.ArgumentParser(
        description="Wrap a flat Eclipse program binary into a bootable 9-track tape image."
    )
    p.add_argument("program_bin", help="flat memory image (dgasm -f bin), native byte order")
    p.add_argument("-o", dest="out", required=True, help="output .tap path")
    p.add_argument("--entry", required=True, type=parse_addr,
                    help="address program_bin's first word loads at, and where "
                         "execution starts (e.g. 050, 0o50, or 40)")
    p.add_argument("--dgasm", default="dgasm", help="dgasm executable (default: dgasm on PATH)")
    args = p.parse_args()

    program_words = le_words_from_file(args.program_bin)
    r1, r2 = build_tape(args.entry, program_words, args.out, args.dgasm)
    print(f"wrote {args.out}: record1={r1}B (boot), record2={r2}B (program) + tapemark")


if __name__ == "__main__":
    try:
        _main()
    except (ValueError, RuntimeError, AssertionError) as e:
        print(f"mktape.py: {e}", file=sys.stderr)
        sys.exit(1)
