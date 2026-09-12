# DG Model 6030 Floppy Diskette Subsystem — `.ab` → `.img` converter

Standalone utility to convert a DG absolute-binary (`.ab`) file into a
raw sector image (`.img`) matching the real DG Model 6030 flexible-
diskette subsystem's geometry, bootable via the 6030's own Program Load
hardware convention. Lives in `eclipse-toolchain/tape/floppy.py` (shared
library, mirrors `mktape.py`'s style and reuses its `.ab` parsing
directly) plus driver scripts `eclipse-ab2floppy` / `eclipse-floppy2ab`
(mirror `eclipse-ab2tape` / `eclipse-tape2ab`'s conventions exactly).

**Bottom line:** real 6030 geometry (77 cyl × 8 sectors × 256
words/sector, single-sided, 157,696 words total) is manual-verified
against a real page image, not OCR. The on-disk boot convention for
**track 0 only** (Program Load hardware auto-loads all 8 sectors of
track 0 to address 0, then hands off at address 0377) is also
manual-verified against a real page image, independently corroborated
by SIMH's own `nova_dkp.c` boot ROM. Everything past track 0 — how a
program bigger than 1792 words would get loaded — is genuinely
undocumented by DG (the manual explicitly leaves "a bootstrap loader
recorded on [the diskette]" up to the customer) and is **not
attempted** here; programs over the limit are rejected, not silently
truncated. Full round-trip (`.ab` → `.img` → `.ab`) is byte-for-byte
lossless, and a real `.ab` file (both hand-assembled and a real
`clang`-compiled C program through this toolchain's actual pipeline)
was booted in this project's own SIMH ECLIPSE build with the DKP device
set to `6030` and its execution verified by examining memory afterward.
None of this has been tried against real 6030 hardware.

## Sourcing

Both PDFs were fetched directly from novasareforever.org's own
documentation index (`archives/documentation/dg.hw/dg_hw_disks`, which
has a `6030_Flexible_Disc/` subdirectory alongside the existing
Zebra/Vulcan/Kismet/Argus `6097_6099_6103_Echo/` one) and saved locally:

- **Primary source, and the one every citation below is from**: DGC
  `014-000065-03`, *Model 6030 Diskette Subsystem Technical Reference*,
  Rev. 03 (1981-Apr), 68 PDF pages. Cited below by the document's own
  page numbers (e.g. "p. II-1") with the PDF page ordinal in parens —
  every citation was checked against the actual rendered page image via
  the `Read` tool's `pages` parameter, not OCR/`pdftotext` (which was
  only used once, early, to locate section headings for navigation, per
  this project's established practice — see `VULCAN_NOTES.md`).
- Also fetched but not needed for this task: DGC `015-000088-00`,
  *DG-Diskette Subsystems Models 6030/6031/6038/6039 Reference* (1978,
  92 pages) — its own "Program Load Logic"/"Program Loading" section
  (p. VI-\*) turns out to describe the **6038/6039 microNOVA-interface**
  controller specifically and defers to a separate *microNOVA
  Computers Programmer's Reference* for the actual bootstrap procedure
  — not applicable to the plain-6030-on-Nova/Eclipse configuration this
  toolchain targets, so `014-000065-03` (which gives the procedure
  directly, for a generic "Data General computer", no microNOVA
  assumption) is the one relied on throughout.
- Independent corroboration (not a citation, a cross-check): SIMH's own
  `~/dev/simh-src/NOVA/nova_dkp.c` models a "moving head disk" (`DKP`)
  controller whose drive-type table includes `TYPE_FLP` ("6030
  (floppy)") and `TYPE_DSDD` ("6097 (DS/DD floppy)", the "quad
  floppy"). This project's own SIMH `ECLIPSE` build (`~/dev/simh-src`,
  built via `make eclipse`, binary at `~/dev/simh-src/BIN/eclipse`)
  compiles `nova_dkp.c` in (see the `ECLIPSE =` line in
  `~/dev/simh-src/makefile`) and has it enabled by default (`show
  devices` lists `DKP     4 units`) — this is a genuine, working
  floppy-controller model, used for real verification below, not just
  read as a fallback source (see "Deep-dive: how the pieces were
  checked" below for exactly what was cross-checked and why the
  convergence is meaningful, not coincidental).

## Real geometry (manual-verified)

DGC `014-000065-03`, Section II "Programmer's Reference Information",
p. II-1 (PDF page 15), "SUMMARY" table, read as a page image:

```
MNEMONIC (FIRST CONTROLLER) ..... DKP
DEVICE CODE (FIRST CONTROLLER)... 33 (octal)
MNEMONIC (SECOND CONTROLLER) .... DKP1
DEVICE CODE (SECOND CONTROLLER).. 73 (octal)
PRIORITY MASK BIT ................ 7
SURFACES/UNIT .................... 1
TRACKS/SURFACE (CYLINDERS) ...... 77
SECTORS/TRACK ..................... 8
WORDS/SECTOR ..................... 256
TOTAL STORAGE CAPACITY (WORDS) . 157,696
```

i.e. single-sided, 77 tracks, 8 sectors/track, 256 words (512 bytes) per
sector — 77 × 8 × 256 = 157,696 words = 315,392 bytes total, which is
`tape/floppy.py`'s `TOTAL_BYTES`. The single-sided claim is independently
confirmed on p. I-8 (PDF page 21)'s NOTE: "While the subsystem
controller may select heads 0-4, the diskette unit can only respond to
head 0 selection."

Independent numeric cross-check (SIMH `nova_dkp.c`, not a citation but a
second, code-derived source): `TYPE_FLP` is defined as `SECT_FLP=8,
SURF_FLP=1, CYL_FLP=77, DKP_NUMWD=256` — exact agreement on all four
numbers, and `show dkp0` in this project's own `eclipse` binary prints
`DKP0    157KW, ...` for a `6030`-typed unit, matching 157,696 words.

The same page (II-1) also gives the DOC ("Specify Disc Address and
Sector Count") and DOA ("Specify Command and Cylinder") accumulator bit
layouts and the command encoding (`00 Read, 01 Write, 10 Seek, 11
Recalibrate`) — this wasn't needed for the converter itself (see
"Design" below — the stage2 relocator does no disk I/O of its own), but
it's an exact match for `nova_dkp.c`'s `FCCY_READ=0/WRITE=1/SEEK=2/
RECAL=3` and `USSC`/`FCCY` bitfield macros, a further convergence
between the manual and the SIMH model worth recording since it's what
gave confidence the SIMH model is a faithful implementation of this
exact manual, not just "a" floppy controller.

## Real boot convention (manual-verified for track 0; undocumented past it)

DGC `014-000065-03`, p. II-9 (PDF page 23), "Program Load (Bootstrap)
from Disc", read as a page image:

> A diskette drive assigned unit number 0 may be a bootstrap device
> using the Program Load Option of DGC computers. To do so, the head
> must be positioned over track 0 and the computer must be Reset...
> Pressing the Program Load switch loads a short program, usually from
> a special ROM in the processor, which initiates a read operation from
> the first sector in track 0. Unless the read operation is terminated
> by the program, reading continues **until 8 sectors from track 0 have
> been transferred from diskette.**

So: Program Load reads the **entire first cylinder** (all 8 sectors,
2048 words) from disk. The same page's Section III cross-reference
points to the "operator's procedure" (p. III-4, PDF page 30,
"BOOTSTRAP PROCEDURE"), which gives the **console-deposit** fallback for
a computer without the Program Load option — and this is what actually
nails down *where in memory* the hardware lands the data and *where*
control ends up, since it's spelling out in words what the hardware
option does automatically:

> 3. Deposit 0601XX₈ in memory location 000376₈, using the console
>    switches. XX is the subsystem device code, 33₈ or 73₈.
> 4. Deposit 000377₈ in memory location 000377₈, using the console
>    switches.
> 5. Start the program at memory location 000376₈.

`0601XX₈` with `XX=33` is `060133₈` — a NIOS-type "start pulse to
device 33" instruction. `000377₈` deposited *at* address `000377₈` is a
`JMP 377` (jump-to-self) instruction. So the manual's own procedure is,
literally: "start the diskette read, then spin at address 0377 until
something changes it." This is **exactly** the same "spin on a
self-referential `JMP` until an in-flight DMA overwrites that exact
word" trap this project's tape path (`tape/mktape.py` /
`tape/boot_stage2.s.in`) already documented and relied on for the
9-track magtape boot ROM — independently reached here from the floppy
manual's own operator procedure, not assumed by analogy.

This is corroborated a third way: SIMH's `nova_dkp.c` `dkp_boot()` (the
boot ROM the simulator itself loads for a `boot dkp0` command) is
literally a 3-word ROM at addresses 0375-0377: `IORST; NIOS DKP; JMP
377` — `IORST` doing what "Press RESET" does in the manual's own
procedure, then the identical `NIOS`+`JMP 377` pair.

Putting the two together: Program Load's hardware DMAs track 0 (2048
words) to memory address 0, and by the time sector 0 alone (the first
256 words, addresses 0-0377) has arrived, address 0377 has been
overwritten with whatever word was in position 255 of that sector —
which is where this project's own stage2 image places a `JMP` to its
real body (see "Design" below), exactly mirroring how tape's record 1
works.

**What is *not* manual-documented**: how a program bigger than what
Program Load's hardware auto-loads (past track 0) is supposed to get
into memory. The manual explicitly punts on this — p. III-4: "It is
assumed that the operator has a diskette with a **bootstrap loader
recorded on it**. This short program will be read into memory and it
will read user programs into memory for subsequent execution" — i.e.
DG expects the customer to have already written their own stage-2 disk
driver, the same way this project had to write `boot_stage2.s.in` for
tape. No DG-authored multi-sector/multi-cylinder disk-driving bootstrap
listing is given in this manual. A stage2 capable of reading track 1
onward would need to drive the DKP-style controller directly (DOC/DOA/
DOB/NIOS/SKPDN, per the same manual's Programmer's Reference on p.
II-1 through II-9) — real register-level disk-driver work in the spirit
of this project's Kismet/Vulcan/Zebra driver-scoping efforts, and **not
attempted here**; see "Design and its limit" below for exactly what
this tool does instead and why.

## Design and its limit

`tape/floppy.py` lays out a `.img`'s track 0 as:

- **Sector 0** (words 0-0377 octal, 256 words): a project-authored
  stage2 "relocator" image, assembled per-build from
  `tape/boot_stage2_floppy.s.in` (the `@ENTRY@`/`@NWORDS@` template
  mechanism mirrors `tape/mktape.py`'s `assemble_stage2()` for
  `boot_stage2.s.in` exactly). Word offset 1 holds the program's real
  entry address, word offset 2 its word count, word offset 0377 the
  `JMP` trap described above.
- **Sectors 1-7** (words 0400-3777 octal, 1792 words): the program's
  raw words, verbatim, always starting at the fixed address 0400 —
  wherever Program Load's hardware happens to land them, regardless of
  the program's real entry address.

Because Program Load's hardware has *already* DMA'd the **entire**
track 0 — sectors 1-7 included — into memory by the time any code can
possibly run (see the timing caveat below for the one real hardware
wrinkle in that claim), stage2 doesn't need to drive the disk
controller at all: it just copies the `nwords` words sitting at the
fixed landing zone (0400) down to the program's real entry address (a
plain `LDA`/`STA`/`INC`/`DSZ` loop — see the assembled/tested listing
below), then jumps there. This is deliberately much simpler than tape's
`boot_stage2.s.in`, which *does* have to issue its own tape read for
"record 2" — floppy's hardware auto-load makes that unnecessary for
anything that fits in one track.

**The limit this creates**: a program must fit in 1792 words (3584
bytes) — track 0's 2048 words minus stage2's own 256-word sector-0
budget. `build_floppy()` raises a clear `ValueError` and refuses to
build an oversized image rather than silently truncating (verified —
see "Verification" below). Supporting a bigger program needs a stage2
that reads track 1 onward itself, which (per "What is not
manual-documented" above) is real, unverified driver work this tool
does not attempt.

**An honest caveat the tape design doesn't share**: tape's record 1 is
*exactly* 256 words, so the boot ROM's DMA of record 1 cannot finish
(and hand off at 0377) before record 1's own last word has arrived —
there's no window for stage2 to start early. SIMH's `nova_dkp.c`
`dkp_svc()` transfers a whole multi-sector command as one atomic
simulated event (confirmed by reading its `do { ... } while
(GET_COUNT(...))` loop, which runs to completion inside a single
service routine with no CPU execution interleaved), so the same is true
in practice against this project's own SIMH build: stage2 cannot begin
executing until *all* of track 0 — sectors 1-7 included — has been
written to memory. Real 6030 hardware, however, is described in the
same manual (p. II-10, "TIMING") as cycle-stealing/rotation-driven — a
sector passes under the head every 20.8ms, and the controller's data
channel requests happen "every two words" — i.e. a real DMA controller
interleaves with CPU execution one word/sector at a time rather than
completing a whole multi-sector transfer atomically. Since address 0377
sits exactly at the sector 0/sector 1 boundary, it's plausible that on
real hardware the CPU's spin-trap could release (and stage2 could start
its copy loop) before sectors 1-7 have actually finished arriving — a
genuine, identified, **unverified** race this design has not been
checked against real timing. Flagged here rather than glossed over, in
the same spirit as `boot_stage2.s.in`'s own tape-vs-real-hardware
caveat.

## Verification

### Round trip (`.ab` → `.img` → `.ab`), byte-for-byte

Two test programs, both round-tripped losslessly with `floppy2ab()`'s
default verification (reassembling `boot_stage2_floppy.s.in` for the
recovered entry/word-count and diffing it against the image's actual
sector 0 — same discipline as `eclipse-tape2ab`'s "recovers the entry
address... verifies by reassembling... and diffing... refuses to guess
on mismatch"):

1. **Hand-assembled test program** (`org 050`; loads a signature
   constant, stores it to a fixed location, then spins) — 6 words,
   `dgasm -t eclipse_s140 -f ab` → `eclipse-ab2floppy` → `.img` →
   `eclipse-floppy2ab` → `.ab`. `cmp` against the original `.ab`:
   **identical**, and the parsed `{address: word}` maps compare equal
   in Python too.
2. **Real `clang`-compiled C program**, through this toolchain's actual
   pipeline (`eclipse-compile.sh`, the same `clang -cc1` → `llvm-link`
   → `opt` → `llc` → `reorder_asm.py` → `dgasm -f ab` sequence every
   other example in this repo uses):
   ```c
   volatile int sink;
   int main(void) { sink = 42; return 0; }
   ```
   compiled to a 26-word `.ab` (entry 050, span 050-0101) — well under
   the 1792-word limit (the first two examples tried,
   `printf_octal_check.c` and `char_test.c`, came out at 1793 and 1915
   words respectively — both over the limit, and `eclipse-ab2floppy`
   correctly rejected them with the "exceeds the 1792-word... budget"
   error rather than truncating, which is itself a useful negative
   test). Round-tripped `.ab` → `.img` → `.ab`: `cmp` **identical**.

### Real boot, in this project's own SIMH `ECLIPSE` build

`~/dev/simh-src/BIN/eclipse`, `DKP0` set to type `6030` (`show dkp0`
confirms `157KW`, matching the manual's 157,696-word capacity exactly):

**Hand-assembled test program**, booted with a breakpoint set at the
program's `halt:` self-loop (address 052 octal):

```
set dkp0 6030
att dkp0 test_prog.img
break 52
boot dkp0
examine 54
examine 52
```
```
Breakpoint, PC: 00052 (JMP 52)
54:	123456
52:	000052
```
PC parked exactly at the expected halt loop, and memory address 054 —
where the program stores its signature constant — holds `123456`
(octal), the exact value the program was built to write. This is a
complete trace through: Program Load hardware track-0 auto-load → the
0377 trap → stage2's relocation copy (from the fixed 0400 landing zone
to the program's real entry address 050) → the relocated program
actually executing correctly.

**Real compiled C program** (`sink = 42;`), booted with no breakpoint
needed (the compiled program itself ends in a `HALT` instruction, which
stops the simulator on its own):

```
set dkp0 6030
att dkp0 tiny.img
boot dkp0
examine 100
examine 101
```
```
HALT instruction, PC: 00057 (ISZ 100)
100:	000052
101:	000000
```
`sink`'s address (0100 octal) holds `000052` octal = 42 decimal —
exactly the value the C source assigns. Full chain verified end to end
with real, unmodified toolchain output: C source → `clang`/`llc` →
`dgasm -f ab` → `eclipse-ab2floppy` → SIMH boot → correct program
behavior.

Not attempted: real 6030 hardware (none available), and a program large
enough to require track-1-onward loading (out of this tool's scope, see
"Design and its limit" above).

## Usage

```
eclipse-ab2floppy [-o output.img] [--entry ADDR] input.ab
eclipse-floppy2ab [-o output.ab] [--no-verify] input.img
```

Same `--entry`/default-to-lowest-loaded-address convention as
`eclipse-ab2tape`/`eclipse-tape2ab`. To boot in this project's SIMH
`ECLIPSE` build:

```
set dkp0 6030
att dkp0 output.img
boot dkp0
```

## Repo layout note (which `eclipse-toolchain` is real)

`git status`/`git log` in `/home/cully/dev/eclipse-package` show it's a
real git repository (`git rev-parse --show-toplevel` →
`/home/cully/dev/eclipse-package`) with `eclipse-toolchain/` tracked
inside it (`git ls-files eclipse-toolchain/tape/` lists
`mktape.py`/`boot_stage2.s.in`). The sibling directory
`~/dev/eclipse-toolchain` is **not** a git repository at all
(`git rev-parse` there fails with "not a git repository") and is a
stale/scratch copy — it's missing `eclipse-ab2tape`, `eclipse-mktape`,
`eclipse-tape2ab`, `eclipse-compile.sh`, `eclipse-run.sh` entirely, and
has `.bak`-suffixed files and a `__pycache__` the tracked copy doesn't.
All of this work (`floppy.py`, `boot_stage2_floppy.s.in`,
`eclipse-ab2floppy`, `eclipse-floppy2ab`) went into
`/home/cully/dev/eclipse-package/eclipse-toolchain/`, the real
git-tracked copy, and is committed there.
