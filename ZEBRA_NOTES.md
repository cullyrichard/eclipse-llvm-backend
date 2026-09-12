# Zebra (DG 6060-series DG-DISC Storage Subsystem) — driver scoping notes

**Post-unification pointer:** `examples/zebra.c` was later refactored
to sit on top of a shared DSKP-family register core
(`examples/dskp_common.h`/`.c`), also used by `examples/vulcan.c` and
`examples/kismet.c`, after all three independently discovered they
share the identical device select code and `DSKP` register convention
this file's own "An unplanned but strong cross-check: Vulcan" section
below already flagged. See `DSKP_FAMILY_NOTES.md` at the repo root for
the unification design, what's shared vs. Zebra-specific, and fresh
verification that the shared-core build still compiles/assembles
cleanly. Everything below this point describes the original,
pre-unification scoping pass and is otherwise unchanged and still
accurate — `zebra.h`'s public API (`zebra_read_block`/
`zebra_write_block`/`zebra_status`) and Zebra's own geometry constants
are unchanged in shape by that refactor.

Prompted by the user's real hardware: they own a physical DG "Zebra"
disk subsystem (Model 6060/6061/6067 DG-DISC Storage Subsystem) and
want block-level driver support for it, toward a Unix-like OS this
project is building toward. This is one of four parallel driver-scoping
efforts this session (Zebra/Vulcan/Kismet/Argus); this file covers only
Zebra, and only touches files this session created
(`ZEBRA_NOTES.md`, `examples/zebra.h`, `examples/zebra.c`) — it does not
modify `STORAGE_NOTES.md`, `MMPU_NOTES.md`, `TRAP_NOTES.md`,
`PMM_NOTES.md`, or the other three devices' `examples/*` files, all
in use by parallel efforts.

**Bottom line, up front:** Zebra's real device select code is **027
octal** (alternate 067 octal), mnemonic **DSKP** in the Technical
Manual's own diagrams, registered DG-wide as mnemonic **DPF** ("DG/Disc
storage subsystem") in the S/140 Programmer's Reference's standard I/O
device code table. It genuinely is a Nova/Eclipse **data channel**
device — but "data channel" turns out to mean the *same* generic
Nova/Eclipse cycle-stealing-DMA convention `DSK` and `DKP` already use
(see `STORAGE_NOTES.md`), not something exotic; Zebra's version is
simply richer (explicit multi-sector burst count, ECC readback, ALT
MODE register overlay, per-drive attention flags, RESERVE/TRESPASS
multiprocessor protocol) than `DSK`'s minimal model, and closely
resembles `DKP`'s seek-then-transfer shape. A driver skeleton
(`examples/zebra.h`/`zebra.c`) exists, implementing
`zebra_read_block(unit, blockno, buf)` / `zebra_write_block(unit,
blockno, buf)`, and **compiles and assembles cleanly** against this
project's real `eclipse-cc` + `dgasm` toolchain — but, like the parallel
Vulcan effort, it has **never executed**, on any simulator or real
hardware. See "What was and wasn't checked" near the end before trusting
any of it.

## Sources consulted

Downloaded directly (not relying on WebFetch's lossy AI-summarized text
conversion — per this project's own established rule, all register/bit
tables below were read from actual rendered page images):

- **Technical Manual, Rev. 3** (DGC 015-000061-03, 1976-1980),
  `015-000061-03__6060_Series_DG-DISC_Storage_Subsystem_Technical_Manual__1976-1980.pdf`
  — 130 pages, mixed Rev. 01/Rev. 02 content (different sections were
  revised at different times; the manual itself is internally
  consistent about which revision stamp appears on which page). This is
  the primary source for everything below.
- **6067 50-MB Technical Manual** (DGC 015-000076-00, 1978) — downloaded,
  not deeply cross-checked against the 411/815-cylinder numbers below
  (out of the time this scoping pass had; flagged explicitly wherever
  it matters).
- **Maintenance Procedures, Rev. 2** (DGC 015-000062-02, 1977-1979) —
  downloaded, not needed for the programming interface (as expected —
  it's a maintenance/alignment manual, not a programmer's reference).
- **Disk Class Handbook** (DGC 052-000049-00, 1981) — downloaded, not
  needed once the Technical Manual's own Appendix A turned out to have
  everything required (see below).
- **ECLIPSE S/140 Programmer's Reference** (DGC 014-000642-02, Rev. 02,
  April 1981), `bitsavers.org/pdf/dg/eclipse/014-000642-02_S140_PgmrRef_Apr81.pdf`
  — already cited by this project's `MMPU_NOTES.md`; used here for the
  general "data channel" concept (Chapter 2, pp. 2-21/2-22) and the
  DG-wide standard I/O device code table (Appendix B, p. B-93). This PDF
  has **no OCR text layer at all** (`pdftotext` produces zero output) —
  every citation to it below was found by paging through rendered
  images directly, not by searching extracted text.

As the task brief predicted: **there is no separate, dedicated
"Programmer's Reference" document for Zebra** anywhere in the
novasareforever.org archive (unlike Vulcan/Kismet, which have one).
Confirmed directly, not assumed: the Technical Manual's own Table of
Contents (p. ii-iii; see below) shows Chapters III-VIII are entirely
drive electromechanics (positioner/servo, MFM data transcription,
spindle/brush, power distribution, subassemblies) — **the complete
programming interface for Zebra lives in exactly one place**: a
44-page appendix, "Appendix A: Detailed Information Supporting Chapter
II" (pp. A-1 through A-44), reached from the very short (5-page)
Chapter II "Controller/Adapter Overview". Page A-1 ("Instruction
Summary") and page A-2 ("Error Conditions") together are, in effect,
Zebra's entire programmer's reference — everything below comes from
those two pages, cross-checked against each other and against the
shorter, lower-resolution "Programming Summary" page at the end of
Chapter I (Rev. 01 content; Appendix A is Rev. 02, redrawn more
legibly — the two agree on every point checked, a genuine two-revision
cross-check, not just one source read twice).

## Real hardware identity and device select code

The Technical Manual's Chapter I "Programming Summary" (unpaginated,
last page of Chapter I) and Appendix A p. A-1 ("Instruction Summary",
Rev. 02, top-right of the page, "A-1 of 44") both give, verbatim:

```
Mnemonic                       DSKP
Device Code               27 (Alt. 67)
Priority Mask Bit                 7
```

**Is "27" octal or decimal?** Not marked with a subscript either way on
this page (DG's own convention elsewhere, confirmed below, *does* mark
this explicitly with an "8" subscript — its absence here is a real gap,
not an oversight in reading). Resolved by cross-reference to two
independent, stronger sources:

1. The S/140 Programmer's Reference states directly: "The ECLIPSE
   S/140 has a **6-bit** device selection network, corresponding to
   bits 10-15 in the I/O instruction format... With a 6-bit device
   code, 64 separate devices can be individually controlled" (Chapter
   2, "Input/Output", p. 2-21) — i.e. the legal range is 0-77 octal (0-63
   decimal). The same manual's ERCC section states "The device code for
   the ERCC facility is **2**8" (p. 5-82.2) — the literal digit "2" with
   an "8" subscript denoting octal, Data General's own explicit
   notation for exactly this ambiguity, used elsewhere in the same
   document.
2. **Appendix B, "Standard I/O Device Codes"** (p. B-93) is a complete,
   unambiguous table, headed "OCTAL DEVICE CODES" in its own column
   title, listing every DG-registered device mnemonic against its
   select code and priority mask bit. It contains, verbatim:

   ```
   OCTAL DEVICE
   CODES   MNEMONIC  PRIORITY MASK BIT   DEVICE NAME
     27      DPF            7            DG/Disc storage subsystem
     33      DKP            7            Moving head disc
     67      DPF1           7            Second DG/Disc storage subsystem
     73      DKP1           7            Second moving head disc
   ```

   This resolves the question directly: **27 and 67 are octal**, and
   they are a *different* select code from `DKP`'s 033 octal (the
   general-purpose moving-head-disk-pack controller this project's
   `STORAGE_NOTES.md` already documents from `nova_dkp.c`) — not a
   coincidental collision with it, as an earlier, wrong hypothesis
   during this investigation briefly considered. Both entries also
   independently confirm priority mask bit 7, matching the Technical
   Manual's own value exactly — a real cross-check between two
   completely different DG documents, not the same fact quoted twice.

**So: Zebra's real device select code is 027 octal** (0x17), **priority
mask bit 7**, mnemonic `DSKP` in the Technical Manual's own diagrams,
registered DG-wide as `DPF` ("DG/Disc storage subsystem" — a generic
product-category name, not a specific model number). **Alternate select
code 067 octal**, mnemonic `DPF1`, for a second controller board in a
dual-processor configuration — not used by this driver (single-
processor scope; see below).

### An unplanned but strong cross-check: Vulcan

Within this same working session, a parallel effort scoped the DG Model
6122 "Vulcan" DG-Disc Storage Subsystem (see `examples/vulcan.h`/
`vulcan.c`, **not modified by this file** — cited here read-only). Its
own, separate Programmer's Reference (DGC 014-000644-00) documents the
**exact same** select code (027 octal, alt 067), the **exact same**
`DSKP` mnemonic, and a **byte-for-byte identical 16-entry command list**
at the **same bit position** (DOA bits 5-8) as this file's own
`ZEBRA_CMD_*` table below (`READ`=0, `RECALIBRATE`=1, `SEEK`=2, `STOP`=3,
`OFFSET FORWARD`=4, `OFFSET REVERSE`=5, `WRITE DISABLE`=6, `RELEASE`=7,
`TRESPASS`=8, `SET ALT MODE 1`=9, `SET ALT MODE 2`=10, `NO OPERATION`=11,
`DATA VERIFY`=12, `READ BUFFERS`=13, `WRITE`=14, `FORMAT`=15). Vulcan's
geometry (815 cylinders × 19 surfaces) is identical to Zebra's own
double-density (6061) geometry, differing only in sectors/track (35 vs.
24) and recording density. This is strong, independent evidence that
"select code 027, mnemonic DSKP" is Data General's standing,
generation-spanning register convention for its entire "DG/Disc Storage
Subsystem" product category — Vulcan reads as a later, higher-density
hardware generation built on the *same* programming model Zebra already
established, not an unrelated coincidence. (Practical implication: a
real system would only ever have one such controller installed at a
time, so DG reusing one select code across hardware generations of the
same product slot makes complete sense — the same way `DSK`/`DKP`'s own
codes are each shared across *their* several supported drive types,
per `STORAGE_NOTES.md`.)

## Geometry and capacity

Chapter I "Summary of Characteristics" / "Capacity and Timing" tables,
and the Programming Summary page, state directly:

```
Surfaces per Unit                    19
Tracks per Surface
  Single Density                    411
  Double Density                    815
Sectors per Track                    24
Bytes per Sector                    512
Capacity per Drive
  Single Density          95,956,992 bytes
  Double Density         190,279,680 bytes
```

("19 Data Surfaces, 1 Servo Surface" per Chapter I's own prose — the
20th, servo, surface is not software-addressable, hence 19, not 20, in
the surface-address field below.)

**Internal consistency check** (arithmetic done here, not printed in
the manual): 411 cylinders × 19 surfaces × 24 sectors × 512 bytes =
**95,956,992 bytes**, exactly matching the manual's own stated
single-density capacity. 815 × 19 × 24 × 512 = **190,279,680 bytes**,
exactly matching the stated double-density capacity. Both check out
exactly — real confirmation that the field widths below (5-bit surface,
5-bit sector, 10-bit cylinder) are being read correctly, not just a
plausible-looking guess.

512 bytes/sector = 256 16-bit words — the same sector size as `DSK`
(`STORAGE_NOTES.md`), a pleasant coincidence that makes `examples/
zebra.c`'s buffer shape identical to `disk_probe.s`'s.

The 6067 (50MB) variant has its own Technical Manual
(015-000076-00) with presumably different sector/track or surface
counts to reach a ~50MB capacity distinct from either density above —
**not independently checked in this session** (downloaded but not read
page-by-page; out of the time this pass had). Confirm against that
document before relying on `ZEBRA_CYLINDERS`/surface/sector counts for
a real 6067 unit specifically.

## Register/command model (from Appendix A, pp. A-1/A-2)

This is a genuine Nova/Eclipse **data channel** device, per the S/140
Programmer's Reference's own definition (Chapter 2, p. 2-21, quoted in
full since it's the clearest primary-source statement of what "data
channel" means on this architecture and this project didn't have it
written down anywhere before now):

> "Data channel I/O permits data transfer in blocks of words, with
> program control necessary only at the start of the operation. The CPU
> stops during each word transfer; but the transfer is made directly to
> or from memory, so no additional steps are required... Data channel
> devices are controlled in three phases. Phase I specifies the
> starting location in memory for the first word to be transferred.
> Phase II loads the two's complement of the number of words to be
> transferred into the machine. These two phases are performed with
> programmed I/O instructions. Phase III issues a flag command. Once a
> flag command is issued, data transfer takes place when both the data
> channel device and the processor are ready. No further program
> control is required."

This is **exactly** the same mechanism `STORAGE_NOTES.md` already
documented for `DSK`/`DKP` from `nova_dsk.c`/`nova_dkp.c` directly
(load address + count/block registers via bare `DOx`, then a pulsed
instruction starts an autonomous transfer, no per-word CPU
involvement) — not a different, exotic interface. Zebra's version maps
onto the same three phases directly: Phase I = `DOB` (memory address),
Phase II = the sector-count field packed into `DOC` (two's complement,
same as the general definition says), Phase III = the `S` flag command
on the final register load. The genuine difference from `DSK` is
richness, not kind: an explicit multi-sector burst count (up to 32
sectors/operation, Chapter I: "Transfers up to 32 sectors per operation"
— this driver only uses 1), an 8-word FIFO buffer between the data
channel and the drive (Chapter I: "8-word data channel FIFO buffer"),
32-bit ECC (readable back via two ALT MODE 2 registers), and a
seek-then-transfer command sequence closer in *shape* to `DKP`'s model
than to `DSK`'s single-register-and-go one — see "Command sequencing"
below.

### DOA — Specify Command and Drive

```
bit   0    1     2     3     4     5-8       9-10          11-15
     R/W  DRV0  DRV1  DRV2  DRV3  COMMAND  DRIVE SELECT   EXTENDED MEM ADDR
```

- Bits 1-4 (`DRV0`-`DRV3`): "1 clears attention flag" for that specific
  drive (independent of the `DRIVE SELECT` field — these are per-drive
  flags addressed directly by bit position, not gated by which drive is
  currently selected).
- Bits 5-8: 4-bit `COMMAND` field. Full list (Appendix A p. A-1, binary
  as printed):
  ```
  0000 READ            0100 OFFSET FORWARD   1000 TRESPASS         1100 DATA VERIFY
  0001 RECALIBRATE     0101 OFFSET REVERSE   1001 SET ALT MODE 1   1101 READ BUFFERS
  0010 SEEK            0110 WRITE DISABLE    1010 SET ALT MODE 2   1110 WRITE
  0011 STOP DISC       0111 RELEASE DRIVE    1011 NO OPERATION     1111 FORMAT
  ```
- Bits 9-10: 2-bit `DRIVE SELECT` (`00`=#0, `01`=#1, `10`=#2, `11`=#3).
- Bits 11-15: extended memory address bits (for >32K/NOVA-3-compatible
  addressing — unused by this driver, same 0-32767 restriction as
  `mmpu.h`'s Phase 1 API and `disk_probe.s`).
- Bit 0, labeled tersely "R/W": **not explained anywhere in Appendix
  A's prose** (there isn't any — it's a reference card, diagrams only).
  Vulcan's own, more legible manual documents the same bit position as
  "Clear R/W Done" (`examples/vulcan.c`'s own comment). Borrowed here by
  analogy, not independently confirmed for Zebra specifically — flagged
  explicitly in `examples/zebra.c`'s own comment on
  `ZEBRA_DOA_CLR_RW_DONE`.
- Caption: "(RESERVES A PREVIOUSLY UNRESERVED DRIVE) (CLEARS ALT MODE)"
  — every `DOA` reserves the addressed drive as a side effect; this
  driver's single-processor scope means that's harmless (see "Scope"
  below).

### DOC — context-sensitive on the previous DOA

If the **previous** `DOA` specified `SEEK` (command `0010`):

```
bits 0-5: unused         bits 6-15: CYLINDER ADDRESS (10 bits, 0-1023)
```

If the previous `DOA` did **not** specify `SEEK`:

```
bit 0        bits 1-5              bits 6-10             bits 11-15
MAP MODE   SURFACE ADDRESS      SECTOR ADDRESS         SECTOR COUNT
           (starting, 5 bits)   (starting, 5 bits)   (2's complement, 5 bits)
```

Both the surface (0-18) and sector (0-23) fields fit directly in 5 bits
each (19, 24 < 32) — unlike Vulcan's `DSKP`, whose 35 sectors/track need
a 6th bit and therefore an extra "Specify Extended Sector and Count"
`DOC` before this one (`examples/vulcan.c`'s `doc1_word`). Zebra needs
no such extra step.

### DOB — Specify Memory Address

```
bit 0: extended memory address LSB (unused by this driver)
bits 1-15: memory address (start of data field)
```

### DIA — Read Data Transfer Status (non-alt mode)

```
bit 0     1        2-5              6    7       8    9      10      11        12      13     14    15
(unused) R/W    DRV0-DRV3(attn)  PAR  INVSEC  ECC  BADSEC  CYLERR  SEC/HDERR  VERIFY  RWTMO  DLATE  RWFLT
```

Bits 6-15 (the fault bits) are **numerically confirmed twice**: once
from the p. A-1 block diagram's callout lines, and independently from
p. A-2's "Error Conditions" table, whose own "INPUT INSTRUCTION/AC BIT"
column names each fault's exact bit number directly. Transcribed from
that table (a real cross-check — the two pages were typeset
separately and agree exactly):

| Bit | Signal name (p. A-2) |
|---|---|
| 6 | PARITY ERROR |
| 7 | INVALID SECTOR |
| 8 | ECC ERROR |
| 9 | BAD SECTOR |
| 10 | CYLINDER ERROR |
| 11 | SECTOR/HEAD ERROR |
| 12 | VERIFY ERROR |
| 13 | READ/WRITE TIMEOUT |
| 14 | DATA LATE |
| 15 | READ/WRITE FAULT |

Bit 1 ("R/W" in the compact Rev. 02 diagram) is very likely **CONTROL
FULL**: the fuller, lower-resolution Rev. 01 Chapter I summary page
draws this exact same bit position's callout line out to a label
reading "CONTROL FULL" — matching the Programming Flowcharts' own
"CONTROL FULL?" decision box (p. A-3), and Chapter I's own prose ("A
controller can store a command for each channel and issue it when the
channel opens"). Not independently bit-numbered in the p. A-2 fault
table (it isn't a fault), so this is a cross-reference inference, not a
direct numeric confirmation — flagged as such in `zebra.c`.

Bits 2-5 (`DRV0`-`DRV3`): per-drive "attention request" flags, "1
indicates attention req" per p. A-1's own caption. This is the
mechanism the Programming Flowcharts use to signal SEEK/RECALIBRATE/
etc. completion (see "Command sequencing" below) — **distinct** from
the Busy/Done pair used for READ/WRITE/FORMAT/READ BUFFERS/VERIFY
completion.

### DIB — Read Drive Status (non-alt mode)

```
bit  0    1    2     3     4    5     6     7        8      9     10    11    12    13    14    15
INVSTAT RSVD TRESP READY BUSY OFFS WRDIS (unused) INVADDR ILLCMD PWR PACKUNSF POSFLT CLKFLT WRFLT DRVFLT
```

Bit 7 is drawn cross-hatched (reserved/unused) on the p. A-1 diagram —
confirmed by the p. A-2 fault table simply having no bit-7 entry
either (the fault-table bit numbers jump 6→8 for `DIB`, wait — actually
jump from a present bit list straight to 8, i.e. no bit 7 signal name
anywhere). Bits 8-15 numerically confirmed the same two-source way as
`DIA`'s fault bits, from p. A-2:

| Bit | Signal name (p. A-2) |
|---|---|
| 8 | INVALID ADDRESS |
| 9 | ILLEGAL COMMAND |
| 10 | POWER FAULT |
| 11 | PACK UNSAFE |
| 12 | POSITIONER FAULT |
| 13 | CLOCK FAULT |
| 14 | WRITE FAULT |
| 15 | DRIVE FAULT |

Bits 0-6 (`INVALID STATUS`, `DRIVE RESERVED`, `TRESPASSED`, `READY`,
`BUSY`, `POSITIONER OFFSET`, `WRITE DISABLED`) come from the p. A-1
block diagram's callout order only — not independently bit-numbered in
the fault table (they aren't faults), but the count matches exactly (15
labeled callouts total, for the 15 usable bits of a 16-bit word minus
the one reserved bit 7 — a real internal consistency check: if the
callout-to-bit mapping were wrong, this count wouldn't come out even).

### S, C, P flag commands (Chapter I; repeated as IOSTART/IOCLEAR/IOPULSE, Appendix A p. A-1)

```
f=S  Sets Busy to 1, Done to 0. Starts: READ, WRITE, FORMAT, READ
     BUFFERS, VERIFY.
f=C  Sets Busy and Done to 0, stops all data transfer operations.
f=P  Starts: SEEK, RECALIBRATE, OFFSET, STOP, WRITE DISABLE, RELEASE,
     TRESPASS. Does NOT affect the Busy flag or Done flag.
```

This last line is load-bearing for the driver: it means `SKPDN`/`SKPBN`
(the polling idiom `disk_probe.s` already proved works for `DSK`)
**cannot** detect SEEK completion on Zebra — a real, manual-documented
difference from the READ/WRITE/FORMAT/READ BUFFERS/VERIFY group, which
*can* be polled that way (`DONE` = "READ/WRITE ATTENTION" per p. A-1's
own definition, the same flag `disk_probe.s` already polls for `DSK`).
SEEK completion is instead signaled via the per-drive `DIA` attention
bits described above.

`IORESET` (p. A-1): "Release reserved drives. Clear positioner offset.
Clear ALT mode. Clear all controller and adapter registers... Select
lowest numbered ready drive and execute recalibrate. Clear Done (R/W
attention)." — this is the *general* Nova/Eclipse I/O reset (`IORST`),
not a Zebra-specific instruction; not implemented separately in
`examples/zebra.c` (a real driver would rely on the same system-wide
reset every other device here does).

## Command sequencing (Programming Flowcharts, pp. A-3/A-4)

Appendix A's own "Command Initiation Flow Chart (Programming)" (p. A-3)
gives two parallel flows, "Drive Command" (SEEK/RECAL/OFFSET/STOP/
WRITE DISABLE/RELEASE) and "Read/Write Command" (READ/WRITE/FORMAT/
READ BUFFERS/VERIFY), plus a second chart (p. A-4) for interrupt/
attention processing. Both charts are **diagrams only** — Appendix A
has no body prose anywhere explaining exactly what each decision box
tests at the register-bit level beyond what's already in the p. A-1
diagrams.

The "Drive Command" flow's own "FOLLOWUP R/W COMMAND?" branch suggests
a SEEK immediately followed by a READ/WRITE can skip straight into the
Read/Write flow's own "CONTROL BUSY?"/"CONTROL FULL?" polling, without
a separate explicit wait for drive attention in between (the controller
appears to defer the R/W command internally until the seek it already
knows about finishes — the same behavior Vulcan's own, prose-equipped
manual states outright: "When the selected drive completes the previous
seek operation... the controller transmits the stored read/write
command to the adapter and the operation begins"). **This driver does
not take that shortcut.** Given no body text anywhere in this specific
manual confirms exactly what "CONTROL BUSY" vs. "CONTROL FULL" each
individually gate, `examples/zebra.c`'s `zebra_seek()` takes the
conservative, unambiguous path instead: issue `DOA` (select+SEEK) +
`DOCP` (cylinder, P-pulsed), then explicitly poll *this specific
drive's* `DIA` attention bit until it's set, then explicitly clear it,
before proceeding to the read/write phase at all. This is slower and
more explicit than the manual's own more-efficient documented path, but
doesn't require guessing at unconfirmed gating semantics. A future
increment revisiting this file could implement the `FOLLOWUP R/W
COMMAND` shortcut once/if those two flags' exact meaning is confirmed
some other way (e.g. against the 6067 manual, not read in this pass, or
the maintenance manual's schematics).

## Driver skeleton: `examples/zebra.h` / `examples/zebra.c`

Implements `zebra_read_block(unit, blockno, buf)` / `zebra_write_block
(unit, blockno, buf)`, single sector (256 words) per call, single-
processor scope (no Release/Trespass/Reserved handling), polled
completion throughout (no interrupt-driven completion — this project
has no interrupt-driven scheduler yet, the same scope line
`STORAGE_NOTES.md` already drew for `DSK`), no ECC-based error
recovery, no multi-sector bursts, no extended/64K memory addressing.
Follows `examples/vulcan.c`'s file shape closely (see that file's own
header comment on `"r"`-constrained inline-asm operands, why the device
code is pasted as a literal `027` rather than a `dev DSKP = 027`
declaration, and why combined-pulse mnemonics like `DOCP`/`DOBS` are
used where the flowcharts show a load and a flag command issued
together) — deliberately, so a reader comparing the two Zebra/Vulcan
files side by side sees the real differences (bit widths, the extra
Vulcan-only extended-sector `DOC`, the seek-completion polling
strategy) rather than incidental stylistic ones.

`zebra_status(unit)` is exposed separately for a caller that wants to
check Ready/Busy/Reserved/fault bits before issuing a command.

Return-value convention (see `zebra.h` for the full comment): `0` =
success; negative = a driver-level failure (bad argument, seek timeout,
not ready, a DIB fault bit); positive = the transfer completed but
DIA's masked error-bits field was nonzero, returned directly so a
caller can decode which fault(s) happened against the tables above.

## What was and wasn't checked

**Checked, with real evidence:**
- Device select code (027 octal / DSKP / DPF, alt 067 octal / DPF1),
  priority mask bit (7) — confirmed against **two independent DG
  documents** (the Zebra Technical Manual and the S/140 Programmer's
  Reference's own Appendix B device-code table), not one source read
  twice.
- Register bit layout for `DOA`/`DOC`/`DOB`/`DIA`/`DIB` — read directly
  from rendered page images (400 dpi renders of the actual scanned
  Technical Manual pages, not OCR text), with the fault-bit portions of
  `DIA`/`DIB` cross-checked against a second, independently-typeset
  table (Appendix A p. A-2 "Error Conditions") that numbers the same
  bits by name.
- Geometry/capacity arithmetic (411/815 cylinders × 19 surfaces × 24
  sectors × 512 bytes) reproduces the manual's own stated byte totals
  exactly, both densities.
- `examples/zebra.c` **compiles and assembles cleanly** against this
  project's real `eclipse-cc` + `dgasm` pipeline (confirmed directly in
  this session: `eclipse-cc -o /tmp/zebra_test.simh zebra.c` completed
  with `eclipse-cc: wrote /tmp/zebra_test.simh` and no warnings, after
  one real bug this same compile step caught and this session fixed —
  see below). This is strictly more than a hand-transcription check: it
  confirms every `asm volatile` block's instruction syntax, operand
  form, and the literal `027` device-code slot are all accepted by the
  real assembler, and that the file links (in the single-translation-
  unit sense `eclipse-cc` supports) with no undefined symbols.
- **A real bug was found and fixed by this compile step**: the first
  draft used `unsigned int tries` with `ZEBRA_SEEK_POLL_LIMIT` = 100000
  as the loop bound. This backend's `unsigned int` is 16-bit (max
  65535) — `eclipse-cc` itself warned `comparison of constant 100000
  with expression of type 'unsigned int' is always true`, meaning the
  original loop would never terminate (silently, since `tries` just
  wraps at 65536). Fixed by widening `tries` to `unsigned long`. Kept
  in this file as a concrete example of exactly the kind of mistake
  compiling (even without running) catches, and disk-probe-style
  hand-assembly does not.

**NOT checked — genuinely unverified:**
- **No execution whatsoever.** This project's SIMH build
  (`~/dev/simh-src/BIN/eclipse`) does not model Zebra's controller at
  all — confirmed directly: no "zebra"/"6060"/"6061"/"6067" reference
  anywhere in `~/dev/simh-src/NOVA/*.c`. There is no way to single-step
  this driver against simulated hardware the way `mmpu_*.s`,
  `trap_probe.s`, or `disk_probe.s` were verified. Real-hardware testing
  (via `eclipse-run.sh` and a real serial connection, per this
  project's `README.md`) is the only way this could actually be
  confirmed to work, and hasn't been attempted.
- DOA bit 0's exact semantics ("R/W", assumed "Clear R/W Done" by
  analogy with Vulcan's own, separate manual — not confirmed for Zebra
  itself).
- DIA bit 1's exact name ("CONTROL FULL", inferred by cross-referencing
  a differently-typeset page of the same manual — not independently
  numbered in the fault table).
- The exact gating semantics of "CONTROL BUSY" vs. "CONTROL FULL" in
  the Read/Write Command flowchart (see "Command sequencing" above) —
  this driver deliberately avoids depending on a guess here.
- The 6067 (50MB) variant's geometry — its own Technical Manual was
  downloaded but not read page-by-page in this session; do not assume
  `ZEBRA_CYLINDERS`/surface/sector counts apply to a real 6067 without
  checking that document first.
- Multi-sector transfers, ECC-based error recovery, RESERVE/TRESPASS
  handling for a real dual-processor configuration, interrupt-driven
  completion, and extended/64K memory addressing — all real Zebra
  capabilities per the manual, all explicitly out of this skeleton's
  scope (see "Driver skeleton" above).

## Regression check

Only new files were added (`ZEBRA_NOTES.md`, `examples/zebra.h`,
`examples/zebra.c`) — no existing file was modified, so existing
baselines are unaffected by construction. `examples/vulcan.c`/
`vulcan.h`, `examples/kismet.*`, and any Argus files were read (for
the cross-check above) but never written to.
