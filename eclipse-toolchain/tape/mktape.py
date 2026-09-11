#!/usr/bin/env python3
"""mktape.py -- wrap a compiled Eclipse program into a bootable 9-track
tape image (SIMH .tap format) for eclipseemu's MTA device (and, in
principle, a real Eclipse's magtape controller -- see this toolchain's
eclipse-mktape header comment for the same real-hardware caveat
eclipse-compile.sh's -f ab path carries: this has only been verified
against eclipseemu, not real tape hardware).

Tape layout (see boot_stage2.s's header comment for the full mechanism
this implements):
  record 1: exactly 256 big-endian words (addresses 0-0377octal). Word 0
            is a dummy (address 0 is never used by anything that runs
            here); words 1-0377 are boot_stage2.s's own assembled image,
            zero-padded if shorter. Word 0377 -- wherever eclipseemu's
            (and real Eclipse hardware's) generic magtape boot ROM ends
            up trapping once this record's DMA overwrites it -- holds
            stage2's own entry jump.
  record 2: the compiled program's raw memory image (dgasm -f bin),
            big-endian words, to be DMA'd starting at address 050 (the
            fixed entry point every program this toolchain builds uses).
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
import struct
import sys

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


def build(stage2_bin_path, program_bin_path, out_path):
    stage2_words = le_words_from_file(stage2_bin_path)
    # boot_stage2.s's own trailing `org 0377 / JMP stage2, 0` produces one
    # word *at* address 0377 (the last of the 255 words available: 1-0377)
    # plus dgasm -f bin's output span running one word past its last
    # written address (address 0400) -- that trailing word is a fixed,
    # always-zero artifact of the span calculation, not real content, and
    # is dropped here; anything beyond that genuinely means boot_stage2.s
    # grew past the address range this design has to fit in.
    body, tail = stage2_words[:255], stage2_words[255:]
    if any(tail):
        raise ValueError(
            f"boot_stage2.s assembled to {len(stage2_words)} words with "
            f"nonzero content past address 0377 ({tail}) -- exceeds the "
            f"255 words available before the boot ROM's trap address"
        )
    record1_words = [0] + body + [0] * (255 - len(body))
    assert len(record1_words) == 256
    record1 = big_endian_bytes(record1_words)

    program_words = le_words_from_file(program_bin_path)
    if len(program_words) > MTA_MAX_RECORD_WORDS:
        raise ValueError(
            f"{program_bin_path}: {len(program_words)} words exceeds the "
            f"{MTA_MAX_RECORD_WORDS}-word single-record limit (see "
            f"boot_stage2.s's 'Known limit' comment)"
        )
    record2 = big_endian_bytes(program_words)

    write_tap(out_path, [record1, record2, None])
    return len(record1), len(record2)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} stage2.bin program.bin out.tap", file=sys.stderr)
        sys.exit(1)
    r1, r2 = build(sys.argv[1], sys.argv[2], sys.argv[3])
    print(f"wrote {sys.argv[3]}: record1={r1}B (boot), record2={r2}B (program) + tapemark")
