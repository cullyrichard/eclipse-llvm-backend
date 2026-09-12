# Kismet (DG-Disk Storage Subsystem, Models 6160/6161/6214) — driver scoping

**Post-unification pointer:** the real build conflict this file's own
"A real cross-driver conflict, found by checking the sibling Vulcan
effort" section documents below was fixed by refactoring
`examples/zebra.c`, `examples/vulcan.c`, and `examples/kismet.c` onto a
shared DSKP-family register core (`examples/dskp_common.h`/`.c`) with a
compile-time variant selector, so exactly one generation's driver is
ever linked into a build. See `DSKP_FAMILY_NOTES.md` at the repo root
for the unification design, the empirical re-confirmation that the
conflict is actually fixed (not just argued), and this file's own two
documented manual inconsistencies (DOC/DIC bit 1, DIB alt-mode-1 bit 4)
carried forward unresolved, exactly as recorded below — not silently
resolved differently by the refactor. Everything below this point
describes the original, pre-unification scoping pass and is otherwise
unchanged and still accurate — `kismet.h`'s public API
(`kismet_read_block`/`kismet_write_block`) and Kismet's own geometry
constants are unchanged in shape by that refactor; `kismet.c`'s
internal register discipline did change (global+`ELDA`/`ESTA` to
shared-function-call "r"-constrained operands) — see
`DSKP_FAMILY_NOTES.md` and `kismet.c`'s own header comment for why that
specific change is safe.

Prompted by a real question: the user owns real physical "Kismet" disk
hardware (DG Models 6160/6161/6214) and wants block-level driver
support for it. This is one of four parallel driver-scoping efforts on
this codebase (Zebra/Vulcan/Kismet/Argus); this file covers Kismet
only, and touches only files this effort owns: this document,
`examples/kismet.h`, `examples/kismet.c`, `examples/kismet_probe.s`.

**Bottom line up front:** the real device select code is **DSKP,
device code `027` octal** (an alternate `067` octal is jumper-
selectable on the controller). The register/command model is a real,
fully-specified 4-phase protocol (select+seek, position, select+read-
write, transfer) documented in detail across pages 3-19 of the
Programmer's Reference — richer than DSK's flat register-and-go model
and DKP's simpler two-register seek+transfer pair (see
`STORAGE_NOTES.md`), but still smaller and more orthogonal than it
first looks once the phase structure is understood. A driver skeleton
(`examples/kismet.h`/`kismet.c`, a C-callable
`kismet_read_block`/`kismet_write_block` pair, plus
`examples/kismet_probe.s`, a hand-written register-level walkthrough)
compiles and assembles cleanly against this project's real toolchain
(`eclipse-cc` through `dgasm`), but **has never been executed** —
this project's SIMH build does not model Kismet at all, so unlike
`disk_probe.s`/`mmpu_probe.s`, nothing here has been single-stepped
against real or simulated hardware. See "What is and isn't verified"
below for the precise boundary.

## Sources

- **Programmer's Reference rev 1** (Data General pub. 014-000654-01,
  Rev. 01, June 1982, "Models 6160/6161/6214 DG/Disk Storage
  Subsystem Programmer's Reference") — the primary source for
  everything in this document. Downloaded and read as actual page
  images (not OCR/text extraction) via the Read tool's `pages`
  parameter, per this project's established rigor standard
  (`MEMORY.md`'s "Verification rigor preference"). All page numbers
  below (`p.N`) are this document's own printed page numbers (footer),
  not PDF page indices — the PDF has 6 unnumbered front-matter pages
  before printed p.3.
- **Programmer's Reference rev 0** (014-000654-00, 1981, 6160/6161
  only, no 6214) — cross-checked pages 3-6 (Introduction, Controller
  Registers, Programming Summary, first Instructions page) against
  rev 1. Identical register model, identical device code (`27(Alt.
  67)`), identical command table — confirms the register model is
  stable across the one revision that exists, not something rev 1
  changed.
- **Maintenance Guide rev 1** (015-000910-01, 1982) — used for two
  independent cross-checks (drive geometry, and a worked octal-
  address example), both below.

## Real device select code

**DSKP, device code `027` octal, alternate `067` octal.**
Programmer's Reference rev 1 p.4, "Programming Summary" box (Figure
1): `Mnemonic DSKP`, `Device code 27(Alt. 67)`, `Priority mask bit 7`.
Identical in rev 0 p.4. This is a genuine DG mnemonic straight from
the manual's own programming summary, not inferred from a simulator
source comment the way `STORAGE_NOTES.md` had to fall back to for
`DSK`'s "4019" identity — Kismet's manual states its own device code
directly. The `067` alternate is a controller jumper for systems
where `027` is already occupied by something else; this driver
hard-codes the primary `027` and does not attempt to make it runtime-
configurable (see "What's not attempted" below).

Confirmed empirically that `dgasm` accepts `dev DSKP = 027` and every
instruction mnemonic this driver uses against it — see "What is and
isn't verified."

## The register model

Kismet is genuinely a different shape of device from both `DSK` and
`DKP` (`STORAGE_NOTES.md`'s two existing SIMH-modeled disks): it's a
*stored-command* controller. Register/data-out instructions **load**
the controller's internal registers; a following pulse (`S`, `C`, or
`P`, appended directly to any of those instructions, e.g. `DOAS`,
`DOCP`, `DOBS`) **executes** whatever command was loaded, and which
pulse means what is *architecturally fixed* (documented once, p.5),
not per-register:

- **`f=S`**: "Sets the Busy flag to 1; sets the Done flag to 0
  (terminates any read/write operation in progress); sets all
  Read/Write error flags to 0. Disables drive attention interrupts
  and starts read/write timeout. Starts read/write operation (read,
  write, format, verify, or read buffers)."
- **`f=C`**: "Sets the Busy and Done flags to 0. Terminates any
  read/write operation in progress; sets all Read/Write error flags
  to 0 and sets Drive Attention flags to 0; does not terminate any
  drive operation currently in progress."
- **`f=P`**: "Starts the drive operation (seek, or recalibrate); does
  not affect the Busy flag or Done flag." (The Programming Summary box,
  p.4, states the same thing in slightly different words: "Starts the
  following operations. SEEK, RECALIBRATE. (Does not affect the Busy
  flag or Done flag.)" — both editions agree on the substance.)
- **`IORST`**: same as `f=C`, plus initiates a recalibrate operation
  on the lowest-numbered ready drive and clears the head address,
  sector address, sector count, and command registers (command
  register defaults to READ; "If a start pulse is issued after an
  IORST a read operation will read in the bootstrap").

(p.5, "Instructions" section header text, quoted verbatim above.)

### Controller registers (p.3, "Controller Registers")

> The controller contains a Read/Write Busy flag, and eight program
> accessible registers:

| Register name | Number of bits |
|---|---|
| Command and drive address | 5 |
| Memory address | 16 |
| Extended memory address | 5 |
| Cylinder address | 10 |
| Surface, sector and count | 16 |
| Error correction checkword | 32 |
| Drive status | 6 |
| Read/write status | 14 |

### DOA — Specify Command, Drive and Extended Address (p.5)

`DOA[f] ac,DSKP`

```
 0  1  2   3-4   5-8    9   10   11-15
CLR CLR  (not  COMMAND  0  DR   EMA MSB's
R/W  SK  used)
```

| Bits | Name | Contents |
|---|---|---|
| 0 | Clear R/W Done | Clears status register's R/W Done + all R/W error flags (DIA bits 6-15) |
| 1-2 | Clear Seek Done (0-1) | Clears the Seek Done flags for drives 0-1 respectively |
| 3-4 | Not used | |
| 5-8 | Command | 0000 Read, 0001 Recalibrate, 0010 Seek, 0011-1000 Reserved, 1001 Set Alt Mode 1, 1010 Set Alt Mode 2, 1011 No operation, 1100 Verify, 1101 Read Buffers, 1110 Write, 1111 Format |
| 9 | Must be 0 | De-selects both drives if 1 |
| 10 | Drive | Selects drive 0-1 |
| 11-15 | Extended Memory Address | MSBs of the extended memory address (BMC only) |

### DOC — three different meanings depending on context (p.6-7)

This is the one genuinely tricky part of the register model, and it's
stated as an explicit *precondition* in the manual, not left implicit:
which fields a `DOC` loads depends on whether the **previous `DOA`**
specified a seek command or not.

**If the previous DOA specified a seek** — `DOC` = "Specify Cylinder"
(p.6):

```
 0    1-2    3-5      6-15
DIAG DIAG  reserved  CYLINDER
MODE FUNC
```

Cylinder is 0-1466 octal (6160/6161, 823 cylinders) or 0-1512 octal
(6214, 843 cylinders) — see "Cross-checks" below for independent
confirmation of the 823/1466-octal figure.

**If the previous DOA did NOT specify a seek** — first `DOC` =
"Specify Extended, Sector and Count" (p.6), *must* be issued before
the second DOC below:

```
 0-3      4      5      6-9    10       11-15
resv   HD MSB  SEC    resv   CNT MSB   resv
              ADDR
              MSB
```

then a **second** `DOC` = "Specify Head, Sector and Count" (p.7):

```
  0      1-5         6-10          11-15
 MAP   HEAD ADDR   SECTOR ADDR   -SECTOR COUNT
```

| Bits | Name | Contents |
|---|---|---|
| 0 | MAP | 1 = mapped BMC address transfers enabled |
| 1-5 | Head Address | Starting head (0-4 octal for 73MB, 0-11 octal for 147MB) |
| 6-10 | Sector Address | Starting sector, together with bit 5 of the 1st DOC |
| 11-15 | Sector Count | Two's complement of sector count, together with bit 10 of the 1st DOC (max 100 octal = 64 decimal) |

**A genuine manual internal inconsistency, flagged rather than
resolved by guessing:** both this DOC's own bit table (p.7) and its
read-back twin `DIC`'s bit table (same page) list bit 1 *twice* — once
as a standalone row ("1 | ----- | Reserved.") and again as the low end
of the "1-5 | Head Address" range immediately below it. The register-
diagram at the top of each box (the boxed field-boundary drawing, not
the prose table) is unambiguous — field boundaries at bits 0, 1, 5, 6,
10, 11, 15, i.e. Head Address genuinely occupies bits 1-5 as one
5-bit field — and that's what this driver's bit math uses. But the
prose table's separate "bit 1 reserved" row is real, printed text,
not a misreading on this project's part; it's simply not resolved by
anything else in the manual. Recorded here rather than silently
"fixed" by picking one interpretation without saying so.

### DOB — Specify Memory Address (p.8)

`DOB[f] ac,DSKP`

```
  0        1-15
EMA LSB  MEMORY ADDRESS
```

Bit 0 is the extended memory address LSB for BMC, or the high-order
address bit for mapped data channel transfers; this driver leaves it
0 throughout (plain 15-bit logical addressing, the same convention
`examples/disk_probe.s` and `examples/mmpu.c` already use — no
extended/BMC-mapped addressing attempted here).

### DIA — Read Data Transfer Status, default context (p.8)

`DIA[f] ac,DSKP`

```
CNT R/W SEEK  --  PAR ILL ECC BAD CYL HD  VFY R/W DAT R/W
FUL DN  DONE          SEC     SEC ERR SEC     TIM LAT FLT
                                       ERR
 0   1  2-3  4-5   6   7   8   9  10  11  12  13  14  15
```

| Bit | Name |
|---|---|
| 0 | Control full |
| 1 | R/W Done |
| 2-3 | Drive 0-1 Done |
| 6 | Parity |
| 7 | Illegal sector |
| 8 | ECC |
| 9 | Bad sector |
| 10 | Cylinder error |
| 11 | Head/sect error |
| 12 | Verify error |
| 13 | R/W timeout |
| 14 | Data late |
| 15 | R/W fault (OR of all faults above, or a drive fault on the currently selected drive) |

`DIA` also has two alternate contexts (Set Alternate Mode 1/2,
loaded via the `DOA` command field): alternate mode 1 reads back the
current memory address instead (p.10), alternate mode 2 reads the
first ECC remainder word (p.10) — used for the ECC error-correction
procedure (p.19-23), not exercised by this driver's read/write
skeleton.

### DIB — Read Drive Status, default context (p.9)

`DIB[f] ac,DSKP`

```
        RDY BSY  WT       POS      DR
                DIS       FLT      FLT
 0-2  3   4   5   6  7-11  12 13-14 15
```

| Bit | Name |
|---|---|
| 3 | Ready |
| 4 | Busy (executing a position command) |
| 6 | Write disable (front-panel switch) |
| 12 | Positioner fault |
| 15 | Drive fault |

`DIB` alternate mode 1 (p.9) reads back extended memory address MSBs
plus the BMC/fixed-disk identifier flags and a **drive-size ID**
scheme worth calling out for a future auto-detect driver: bits 2&6
identify drive 0's size, bits 3&7 identify drive 1's:

| DIB bit (low) | DIB bit (high) | ID Mbytes |
|---|---|---|
| 0 | 0 | 147 |
| 0 | 1 | 600 |
| 1 | 0 | 73 |
| 1 | 1 | Reserved |

This driver does not use it (geometry is passed in by the caller via
`heads`, see `kismet.h`), but it's a real, cheap way a future version
could self-configure instead of hard-coding a drive model.

**Second inconsistency, same shape as the DOC/DIC one above:** `DIB`
alternate mode 1's prose (p.9) states plainly "Also places the
extended head count in bit 4 of the specified accumulator," but the
same page's own bit table lists bit 4 as `----- | Reserved.` This
field isn't used by this driver at all (it only matters for drive
geometries needing a head-count MSB beyond the plain 5-bit head field
— see "What's not attempted"), so it wasn't load-bearing enough to
chase further, but it's recorded here for the same reason as the
DOC/DIC one: real, printed, unresolved by anything else in either
Programmer's Reference revision.

### Fault flags (p.18) and error conditions (p.19)

Three tiers, matching `DIA`/`DIB` above: **Positioner faults** (`DIB`
bit 12 — illegal cylinder, drifted off-cylinder, guard-band issues),
**Drive faults** (`DIB` bit 15 — DC voltage, R/W-and-not-on-cylinder,
head select, write fault), **Read/Write faults** (`DIA` bits 6-14 —
parity, illegal sector, ECC, bad sector, cylinder/sector-address
mismatch, verify, timeout, data late). p.19's "During Data Transfer"
section is explicit about termination semantics this driver relies
on: "If the subsystem detects a parity or data late error, or a
drive fault, the data transfer operation terminates when the error
occurs." and "If the subsystem detects an ECC or verify error, the
data transfer operation terminates at the end of the sector in which
the error occurred and the sector count indicates the address of the
next sector."

## Programming sequence this driver implements (p.11-12, "Programming Details")

The manual gives this as an explicit 4-phase procedure, and Figures
2-5 (p.12, 13, 14, 15) are flowcharts for it. Quoting the phase
summary directly (p.11):

> I. Select a drive and specify a seek command.
> II. Specify a cylinder address and initiate the position operation.
> III. Select a drive and specify a read/write command.
> IV. Specify a starting memory address, head address, sector address
> and sector count and initiate the read/write operation.

Two details that shaped this driver's control flow directly, both
quoted from p.11-12 because they're easy to get wrong by assuming a
naive fully-synchronous seek-then-transfer model:

- **A `P`-pulsed seek does not touch the Busy/Done flags at all**
  (quoted above under "f=P"). There is nothing to poll for seek
  completion using the standard Busy/Done idiom.
- **The driver is explicitly told not to wait for the seek before
  moving on**: "If a read/write operation is to follow, proceed
  immediately to Phase III without waiting for a drive attention
  interrupt request." The *controller itself* is documented (p.12,
  Phase IV prose) to wait for the drive's Seek Busy flag to clear
  internally before it actually begins the stored read/write command:
  "When the selected drive completes the previous seek operation and
  clears the associated Seek Busy flag, the controller begins
  executing the stored read/write command."

So the correct polling model — and the one `kismet.c` implements — is:
issue the seek (`P` pulse, no poll), immediately issue the read/write
setup and `S` pulse, then poll *only* the standard controller
Busy/Done flag (`SKPDN DSKP`) for the read/write's own completion —
the same generic Nova/Eclipse polled-completion idiom
`STORAGE_NOTES.md`'s `disk_probe.s` already used for `DSK`. This
driver does add one synchronous step the flowcharts leave as an
interrupt-driven exit in the general case: a single (non-looping)
`DIB` read right after Phase I's `DOA`, checked in C for the Ready
and Drive Fault bits before anything else proceeds — matching the
Phase I flowchart's own "Ready? No -> exit to user level" branch,
just resolved synchronously rather than by waiting for an interrupt.

## Cross-checks against a second, independent source

Per this project's stated rigor standard, two arithmetic cross-checks
against the Maintenance Guide (a physically separate document from
the Programmer's Reference, written for field-service engineers, not
programmers):

1. **Geometry.** Maintenance Guide p.1-1 (PDF page 19), Section 1.1:
   "Each data surface has two heads, 823 tracks and 35 sectors per
   track." This independently confirms the Programmer's Reference's
   own table (p.3): 823 cylinders, 35 sectors/track for the 73/147 MB
   drives. And the arithmetic checks out exactly: 823 cylinders
   numbered 0-822 decimal = 0-1466 octal (823/8=102r7, 102/8=12r6,
   12/8=1r4, 1/8=0r1 → 1466 octal), matching the Programmer's
   Reference's own "Cylinders 823 (0-1466₈)" table entry (p.3)
   digit-for-digit.
2. **Sector addressing is a plain decimal-to-octal conversion, not a
   remapped/interleaved value, as seen by software.** Maintenance
   Guide p.4-27 (PDF page 157), "BMC MODE EXAMPLE": "The controller
   is in the BMC mode and decimal cylinder 15, head 2, sector 10 is
   bad. The controller will convert from decimal to octal. The
   program will print out physical cylinder 17, head 2, sector 12 as
   bad" — 10 decimal = 12 octal, a trivial but real confirmation that
   the sector-address field this driver writes is a direct integer,
   not something requiring a software interleave table the way
   `STORAGE_NOTES.md` found `DKP`'s underlying geometry needs. The
   *physical* interleave is entirely inside the controller/drive,
   invisible to the sector-address field software writes: Maintenance
   Guide p.4-26 (PDF page 156), Section 4.4, "FLAGGING MEDIA DEFECTS":
   "The controller reads sectors physically consecutive in the BMC
   mode of operation... The controller reads logically consecutive
   but physically every other sector (interleaving) in the DCH mode
   of operation" — i.e. interleave is a *transfer-mode* property (BMC
   vs DCH), handled entirely by the controller, not something the
   sector-address register's own encoding has to account for.
3. Table 4-10 (Maintenance Guide p.4-28, PDF page 158) gives a full
   0-34 decimal / 0-42 octal physical-address conversion table for BMC
   mode, confirming the sector range end-to-end (0-34 decimal = 0-42
   octal), matching `KISMET_SECTORS_PER_TRACK` and the Programmer's
   Reference's own "Sectors 35 (0-42₈)" (p.3).

## What is and isn't verified

**Verified directly, with evidence:**

- Every register bit layout and command code above: read from actual
  page images of the real Programmer's Reference (both revisions),
  not OCR or paraphrase, with page numbers cited throughout.
- Device code `027` octal: stated directly in the manual's own
  Programming Summary (both revisions) — not inferred.
- Drive geometry (823 cylinders, 35 sectors/track): independently
  cross-checked between the Programmer's Reference and the
  Maintenance Guide, with the octal conversion re-derived and checked
  by hand (see "Cross-checks" above).
- **`dgasm` accepts every mnemonic this driver uses.** Confirmed
  directly against the real `dgasm` binary
  (`~/dev/dgasm-src/dgasm`), independent of any specific device
  model (dgasm's mnemonic grammar doesn't know or care what device
  code a `dev` line names): `DOA`/`DOAS`/`DOAC`/`DOAP`,
  `DOB`/`DOBS`/`DOBC`/`DOBP`, `DOC`/`DOCS`/`DOCC`/`DOCP`,
  `DIA`/`DIAC`, `DIB`/`DIBC`, `DIC`/`DICC`, `SKPBN`/`SKPDN`,
  `NIOS`/`NIOP`/`NIOC` all assembled cleanly in a standalone test
  file targeting `dev DSKP = 027` before being used in
  `kismet_probe.s`.
- **`examples/kismet_probe.s` assembles cleanly** (`dgasm -t
  eclipse_s140 -f simh`, exit 0) against the real assembler, and its
  computed register-word constants were independently re-verified by
  hand (see the "Cross-checks" section's octal arithmetic) and by
  inspecting the assembled output directly — the `dep 000200`
  through `dep 000206` lines in the assembled `.simh` file match the
  hand-computed `doa_seek`/`doa_write`/`doc1_extsec`/`doc2_headsec`
  values exactly (`0000400`, `0003400`, `0000040`, `0000037`).
- **`examples/kismet.c` compiles and assembles cleanly through this
  project's real `eclipse-cc` pipeline** (`clang -cc1` → `llvm-link`
  → `opt -internalize,globaldce` → `llc` → `reorder_asm.py` →
  `dgasm -t eclipse_s140 -f simh`), producing a loadable `.simh`
  image, with no warnings or errors.

**NOT verified, and not claimed to be:**

- **Nothing here has ever run.** This project's SIMH build
  (`~/dev/simh-src/BIN/eclipse`) does not model Kismet/DSKP at all.
  Re-confirmed directly for this writeup: `grep -ril
  "kismet\|6160\|6161\|6214" ~/dev/simh-src/NOVA/*.c` returns two
  files (`nova_sys.c`, `eclipse_cpu.c`), but both hits are numeric
  coincidences unrelated to Kismet — `0061601`/`0061401`/etc. are
  unrelated Nova opcode-table constants that merely contain the
  substring "6160"/"6161", and `262144` (a memory-size constant, 256K
  words) contains "6214" as a substring. Neither file has a disk
  driver, a device named DSKP, or a comment referencing the Kismet
  hardware family — `~/dev/simh-src/NOVA/` has exactly two disk
  device source files, `nova_dsk.c` and `nova_dkp.c`
  (`STORAGE_NOTES.md`'s DSK/DKP), and neither one is Kismet. Running
  `kismet_probe.s` or a `kismet_read_block`/`kismet_write_block` call
  against this SIMH build would not fail loudly — it would hang
  forever in the `SKPDN DSKP` poll loop, because device `027` is
  simply unattached and Done never sets. That's not a meaningful
  test, so it was not attempted (this is a deliberate choice, not an
  oversight — running it and reporting "it hangs" would tell us
  nothing the source-grep above didn't already establish more
  directly).
- **Whether a bare (un-pulsed) `DIA`/`DIB`/`DIC` clears status is an
  assumption, not confirmed.** `disk_probe.s`'s equivalent claim for
  `DSK` was backed by reading `nova_dsk.c`'s source directly
  ("no pulse, does NOT clear status (only a pulsed form does)"). No
  such source exists for Kismet/DSKP — the Programmer's Reference
  documents each instruction's *effect on the loaded data*, but does
  not explicitly state whether reading without a pulse leaves status
  bits set or clears them. This driver reads status with bare `DIA`/
  `DIB` (following the same pattern `disk_probe.s` used for `DSK`,
  on the theory that Kismet's `f=C` semantics — "Sets the Busy and
  Done flags to 0... sets all Read/Write error flags to 0" — describe
  what pulse `C` specifically does, implying a bare read does *not*
  do that on its own) but this is inference from the surrounding
  text, not a directly quoted guarantee.
- **The two manual inconsistencies noted above (DOC/DIC bit 1, DIB
  alt-mode-1 bit 4)** are recorded, not resolved. This driver's bit
  math follows the field-boundary diagrams (which are internally
  self-consistent) rather than the prose tables' apparently-erroneous
  duplicate/contradictory rows, but that choice is unverified against
  any third source.
- **Real hardware behavior of the phase-sequencing shortcut** ("proceed
  immediately to Phase III without waiting") — this driver takes the
  manual's own text as authoritative and does not poll for seek
  completion at all, but that specific claim has no independent
  verification (no simulator, no second manual passage) beyond the
  one paragraph quoting it.
- **Error recovery** (ECC correction per p.19-23's algorithm, retry
  logic, bad-sector handling) is entirely unattempted — this driver
  returns the raw final `DIA` status word on a fault and does nothing
  further.
- **The 6214's wider head field** (40 heads needs 6 bits; the base
  `DOC` "Head Address" field is only 5 bits, bits 1-5) is handled in
  `kismet.c` by shifting a computed head-MSB bit into DOC1's bit 4
  ("HD MSB") per the register table — this is exercised by the code
  path (the shift logic runs for any `heads` value) but was never
  tested against an actual 6214, and the DIB-alt-mode-1 "extended
  head count" inconsistency noted above means even the *read-back*
  side of a wide head number is ambiguous in the manual itself.

## What real-hardware testing would still need (per the task's own scope note, and this project's `README.md`)

Since SIMH genuinely cannot verify this, the only path to real
verification is `eclipse-run.sh` against the user's actual Kismet
hardware over a real serial connection (`README.md`'s own description
of the real-hardware toolchain path, `-f ab` output format instead of
`-f simh`). Concretely, in order:

1. Confirm the real device select code the *user's specific*
   controller is jumpered to (`027` primary vs `067` alternate) —
   this driver assumes `027` and has no way to verify that assumption
   without physical access to the jumper or a live `DIB`/`DIA` probe.
2. A minimal single-`DIB`-read smoke test (select drive 0, read
   status, halt, examine the accumulator over the serial console) to
   confirm the device responds at all before trusting anything else.
3. `kismet_probe.s`'s actual write-then-readback sequence, run for
   real, examining `wstatus`/`rstatus`/`dib_status` and the `rbuf`
   marker words afterward (the exact same verification shape
   `disk_probe.s` already achieved for `DSK`, just requiring real
   hardware instead of SIMH).
4. Only after that: the two open bit-table ambiguities above, and the
   phase-sequencing shortcut, could be considered actually confirmed
   rather than manual-text-only.

## A real cross-driver conflict, found by checking the sibling Vulcan effort

While finishing this file, `VULCAN_NOTES.md` (a parallel driver-scoping
effort on this same codebase, Model 6122) was already committed and
worth a quick read for anything that touches Kismet. It does: Vulcan's
own device is *also* mnemonic `DSKP`, device code `027` octal, alt
`067` octal — identical to Kismet's, not a coincidence of naming but
apparently the same `DSKP` device-class mnemonic and select code
reused across successive DG disk-controller generations (`VULCAN_NOTES.md`
line ~69's own Programming Summary transcription: `Device code 27
(Alt. 67)`). Confirmed directly (not assumed) that this is a real
build-time conflict, not just a documentation coincidence: assembling
two `dev DSKP = 027` declarations together (the shape both
`examples/kismet.c` and `examples/vulcan.c` each emit via file-scope
`asm(...)`, the same mechanism `examples/mmpu.c` established) fails
hard —

```
$ dgasm -t eclipse_s140 -f simh -o dupdev.simh dupdev.s
Multiple definitions for symbol DSKP.
```

— even though both declarations name the identical device code. So
**`examples/kismet.c` and `examples/vulcan.c` cannot both be linked
into one `eclipse-cc` program as they stand today.** In practice this
mirrors the real hardware constraint reasonably well (a system would
plug in one `DSKP`-class controller, not both a Vulcan and a Kismet
at once), but it's a real limitation of these two skeletons as
written, not just a hypothetical — worth fixing (e.g. a build-time
`#define`-selected device code, or renaming one driver's internal
`dev` symbol away from the bare `DSKP` mnemonic) before either one is
used in a real multi-driver OS build. Not fixed here since it's
cross-cutting between two independently-owned files this task was
scoped not to touch.

## What's not attempted

- No filesystem, no partition table, no bad-sector-aware allocation —
  this is a raw block driver only, matching the task's own stated
  first-target scope.
- No runtime drive-size auto-detect via `DIB` alternate mode 1's ID
  bits (documented above as a real, available mechanism this driver
  doesn't use).
- No interrupt-driven completion (Drive Attention interrupts) —
  polling only, same simplification `STORAGE_NOTES.md`'s `DSK` driver
  made for the same reason (no interrupt-driven scheduler exists yet
  in this project).
- No `067` alternate-device-code configurability.
- No multi-sector transfers (the sector-count field supports up to
  64 sectors per operation; this driver always transfers exactly 1).

## Regression check

Only new files were added (`examples/kismet.h`, `examples/kismet.c`,
`examples/kismet_probe.s`, this document) — no existing file was
modified, so existing baselines are unaffected by construction.
