# Vulcan (DG Model 6122 DG-Disc Storage Subsystem) — driver scoping

Prompted by a real question in service of the larger OS-scoping effort:
the user owns real physical "Vulcan" disk hardware and wants block-level
driver support for it. This is one of four parallel driver-scoping
efforts (Zebra/Vulcan/Kismet/Argus); this file covers Vulcan only, and
touches only `VULCAN_NOTES.md` (this file) and `examples/vulcan.h`/
`examples/vulcan.c` — no other agent's files were read or modified.

**Scope note, stated up front so nothing below overclaims it later**:
this project's SIMH build (`~/dev/simh-src/BIN/eclipse`) does **not**
model Vulcan's controller — confirmed by grepping every `.c` file under
`~/dev/simh-src/NOVA/` for `vulcan`/`6122`/`dskp`: no hits. Every other
hardware-facing probe in this repo (`disk_probe.s`, `mmpu_probe.s`,
`trap_probe.s`) was single-stepped against a real SIMH model of the
device in question and its output was checked bit-for-bit. **That is
not possible here.** What follows instead is: (1) a citation-heavy
transcription of the real register/command/status model from the
Programmer's Reference's own page images (not OCR — see "Sourcing"
below), (2) a driver skeleton built to match that model as literally as
possible, (3) the internal-consistency checks that *are* available
without hardware (arithmetic cross-checks against the drive's own
stated geometry/capacity, and confirming the real toolchain compiles
the driver to the intended instruction sequence), and (4) an explicit,
unpadded list of what real-hardware testing would still be needed. If
you take one thing from this file, take that list — everything above
it describes a documented design, not a proven one.

## Sourcing

Both manuals named in the task were fetched directly (not through
WebFetch's summarizer) and saved locally:

- **Programmer's Reference**: `014-000644-00`, *Model 6122 Series
  DG/Disc Storage Subsystem — Programmer's Reference Series*, Data
  General Corporation, First Edition (First Printing, December 1979),
  Rev. 00. 40 PDF pages total — it turns out to be a compact "Programming
  Summary" foldout card (pp.1-32 of content plus DG sales-office/reply
  pages) rather than a large prose manual, which is actually ideal for
  this task: nearly every page is a register bit-table or a command
  description, at real page-image resolution. Every register/command/
  status claim below cites a `pgmref.pdf` page number from this
  document, viewed as rendered page images via the `Read` tool's
  `pages` parameter (never OCR text — `pdftotext` was only used once,
  early, to locate section headings for navigation, and its output was
  visibly garbled, exactly as expected for a scanned/OCR'd 1979
  document — nothing here is sourced from it).
- **Maintenance Manual**: `015-000107-00`, *6122 DG-Disc Subsystem
  Maintenance Manual*, 1980. 264 pages. Downloaded but **not read
  page-by-page** for this pass — the Programmer's Reference alone
  turned out to fully specify the programming model (registers,
  commands, addressing, status/fault bits, and a complete worked
  programming sequence), which is everything a block driver needs. The
  Maintenance Manual would matter for drive geometry *confirmation* at
  the component level (servo/head detail) and for hardware fault
  diagnosis beyond the fault *names* the Programmer's Reference already
  gives — out of scope for a raw block read/write driver, flagged here
  rather than silently skipped.

## Real hardware identity and device select code

**DG Model 6122 Series DG/Disc Storage Subsystem. Mnemonic `DSKP`.
Device select code `027` octal, with an alternate select code `067`
octal.** Straight from the Programming Summary's own header table
(`pgmref.pdf` p.2):

```
Mnemonic                      DSKP
Device code                   27 (Alt. 67)
Priority mask bit             7
Surfaces/unit                 19
Tracks/surface                815
Sectors/track                 35
Bytes/sector                  512
Capacity/drive                277,491,200 bytes
Transfer rate                 1,209,600 bytes/sec
```

**On "Alt. 67"**: the manual states this pairing directly but does not
spell out *why* a second code exists on this same page. `027 + 040
(octal) = 067` — this is the standard Data General convention (used
elsewhere in this project's own device set — see `eclipse_io.h`'s
device-code note) for a jumper-selectable "alternate" device code,
letting two controllers of the *same* type coexist in one system (one
at its primary code, the second jumpered to the alternate). This
reading is **inferred from the standard DG convention, not from
explicit prose in this document** — flagged rather than presented as
manual-confirmed. It doesn't matter for a single-controller system:
this driver uses `027` only.

Up to **4 drives per controller** (unit select 0-3 — confirmed below in
the DOA "Drive" field, and directly stated in the Introduction,
`pgmref.pdf` p.3: "This disc subsystem includes a maximum of four, free
standing, moving head disc drives").

## Geometry, capacity, and a real internal-consistency check

`pgmref.pdf` p.3 ("Introduction"):

```
Surface    19 (0-22_8)
Cylinders  815 (0-1456_8)
Sectors    35 (0-42_8)

Bytes/track     17,920
Bytes/cylinder  340,480
Bytes drive     277,491,200
```

Cross-check (the "sanity-check the register bit-widths against the
drive's real geometry" the task asked for): 19 surfaces × 815 cylinders
× 35 sectors × 512 bytes/sector =

```
19 * 815 = 15,485
15,485 * 35 = 541,975            (sectors/drive)
541,975 * 512 = 277,491,200      bytes/drive
```

This lands on the manual's own stated `277,491,200` bytes/drive
**exactly** — a genuine arithmetic cross-check, not just a
transcription, and it's the number `examples/vulcan.h`'s
`DSKP_SECTORS_PER_DRIVE` (541,975) is built from.

Register field widths, cross-checked against these ranges (both from
`pgmref.pdf` p.2's bit-format diagrams and p.5-p.7's per-field tables):

- **Cylinder**: 10-bit field (`pgmref.pdf` p.5, "Specify Cylinder"
  bits 6-15). Max cylinder needed is 814 decimal (`1456` octal); 10
  bits covers 0-1023 (`1777` octal) — comfortably sufficient, no
  overflow risk.
- **Surface**: 5-bit field (p.6, bits 1-5 of the second "Specify
  Surface, Sector and Count" `DOC`). Max surface needed is 18 decimal
  (`22` octal); 5 bits covers 0-31 (`37` octal) — sufficient.
- **Sector**: **6-bit field, split across two instructions** — this
  is the one place a naive single-table read would get the width
  wrong. The second `DOC`'s own 5-bit "Sector" field (bits 6-10,
  p.6) is captioned *"together with bit 5 of the first `DOC`, selects
  the starting sector (0-42₈)"* — i.e. the real sector address is 1
  MSB (first `DOC`, bit 5, "Sector Address MSB", p.6) + 5 LSBs (second
  `DOC`, bits 6-10) = 6 bits total, correctly covering 0-34 decimal
  (`42` octal, 35 sectors). A 5-bit-only reading would cap out at 31
  decimal and silently corrupt any access to sectors 32-34 — caught by
  actually cross-referencing the two DOC tables' captions against each
  other, not by reading either table alone.
- **Sector count**: same 6-bit split-field shape (first `DOC` bit 10
  "Sector Count MSB" + second `DOC` bits 11-15), holding the **two's
  complement** of the sector count, max magnitude `100₈` = 64 decimal —
  matches the Introduction's own claim (`pgmref.pdf` p.3): "The drives
  can transfer up to 64 consecutive sectors (32,768 bytes) in a given
  cylinder in one operation."
- **Extended memory address**: 5 MSB bits (`DOA`, bits 11-15, p.4) + 1
  LSB bit (`DOB`, bit 0, p.7) + 15-bit memory address (`DOB`, bits
  1-15, p.7) = **21 bits total**, matching the Introduction's own
  claim (p.3): "The burst multiplexor channel controller contains a
  21-bit address register that specifies either physical or mapped
  addresses." (`2^21` = 2,097,152 words addressable via the BMC — far
  past this backend's own 32,768-word logical ceiling, the same
  ceiling `MMPU_NOTES.md` documents from the CPU side. This driver
  does not use extended addressing at all — see "Scope" below.)

## Controller registers (`pgmref.pdf` p.3, "Controller Registers")

```
Register Name                  Number of Bits
Command and drive address      6
Memory address                 16
Extended memory address        5
Cylinder address                10
Surface, sector and count      17
Error correction checkword     32
Drive status                   16
Read/write status              16
```

Two independent channels — one for drive commands, one for read/write
data transfers — let a seek on one drive overlap a transfer on
another; not exploited by this driver (single outstanding operation
only), noted as a real capability left on the table.

## Instruction/register model (all six citations below are direct
transcriptions of `pgmref.pdf`'s own bit tables, not paraphrase)

All instructions target `ac,DSKP` (device `027`). DG bit-numbering
throughout: **bit 0 is the MSB** (value `0x8000` in a 16-bit word),
bit 15 is the LSB (value `0x0001`) — the same convention
`MMPU_NOTES.md`'s `LMP` bit table already uses.

### `DOA[f] ac,DSKP` — Specify Command, Drive and Extended Address (p.4)

```
Bits    Name                    Contents or Function
0       Clear R/W Done          Clears the status register's R/W Done flag.
                                 Clears all R/W error flags except R/W timeout.
1-4     Clear Atten (0-3)       Clears the drive attention flags for drives 0-3.
5-8     Command                 0000 Read            1000 Trespass
                                 0001 Recalibrate     1001 Set alternate mode 1
                                 0010 Seek            1010 Set alternate mode 2
                                 0011 Stop drive      1011 No operation
                                 0100 Offset forward  1100 Verify
                                 0101 Offset reverse  1101 Read buffers
                                 0110 Write disable   1110 Write
                                 0111 Release         1111 Format
9,10    Drive                   Selects drive 0-3.
11-15   Extended Memory Address Specifies the MSBs of the extended memory address.
```

### `DOC[f] ac,DSKP` — Specify Cylinder (p.5)

Context: the previous `DOA` specified a seek operation.

```
Bits    Name       Contents or Function
0-5     ----       Reserved for future use.
6-15    Cylinder   Specifies the desired cylinder 0-1456₈; a seek operation.
```

### `DOC[f] ac,DSKP` — Specify Extended Sector and Count (first `DOC`, p.6)

Context: the previous `DOA` did *not* specify a seek operation. **Must
be issued before the second `DOC` below** (the manual's own `NOTE`,
p.6).

```
Bits    Name                Contents or Function
0-4     ----                Reserved for future use.
5       Sector Address MSB  MSB of the starting sector address for a
                             read/write/format/verify operation.
6-9     ----                Reserved for future use.
10      Sector Count MSB    MSB of the two's complement of the number of
                             sectors to be transferred (max 100₈).
11-15   ----                Reserved for future use.
```

### `DOC[f] ac,DSKP` — Specify Surface, Sector and Count (second `DOC`, p.6)

```
Bits    Name            Contents or Function
0       MAP             If 1, specifies BMC (mapped) addressing.
1-5     Surface         Starting surface (head), 0-22₈.
6-10    Sector          With bit 5 of the first DOC: starting sector, 0-42₈.
11-15   Sector Count    With bit 10 of the first DOC: two's complement of
                         sector count (max 100₈).
```

### `DIC[f] ac,DSKP` — Read Surface, Sector and Count (p.7)

Readback of the second `DOC` register (current, not necessarily
starting, values — same bit layout as above, MAP/Surface/Sector/Count).

### `DOB[f] ac,DSKP` — Specify Memory Address (p.7)

```
Bits    Name                     Contents or Function
0       Extended memory address  LSB of the extended memory address.
1-15    Memory address           Starting address for non-mapped transfers,
                                  or low-order 15 bits for mapped transfers.
```

### `DIA[f] ac,DSKP` — Read Memory Address (alternate mode 1) / Read Data
### Transfer Status (normal) / Read First ECC Word (alternate mode 2)

Three different readbacks share the same instruction, selected by a
prior "Set alternate mode 1/2" command (`DOA` command field `1001`/
`1010`) — cleared again by the next command or an `IORST` (p.19). This
driver only ever uses the **normal** (no alternate mode active) form,
Read Data Transfer Status (p.9):

```
Bit   Name              Contents or Function
0     Control full      Command channel busy; a previous DOA hasn't been
                         transmitted to the adapter yet.
1     R/W Done          Read/write operation from the previous S command
                         terminated. Same as the device Done flag.
2-5   Drive 0-3 Done     Drive executed/rejected a positioner command,
                         was trespassed on, or changed ready status.
6     Parity            Parity error, controller<->adapter transfer.
7     Illegal sector     Sector address exceeded drive capacity.
8     ECC               Data error detected by error-check circuits.
9     Bad sector        Bad sector flag found during header check.
10    Cylinder error    Cylinder address mismatch during header check.
11    Surf/sect error   Surface or sector address mismatch during header check.
12    Verify error      Data mismatch during a verify operation.
13    Read/write timeout Operation not completed within 1 second of START.
14    Data late         FIFO overflow (read) or underflow (write).
15    Read/write fault  Any of the above faults, or a drive fault on the
                         drive currently selected by the R/W channel.
```

### `DIB[f] ac,DSKP` — Read Drive Status (normal form used by this driver, p.9)

```
Bit   Name             Contents or Function
0     Invalid status   Command channel busy; bits 5-6,8-15 should be ignored.
1     Reserved         Drive is reserved by the other processor.
2     Trespassed       Drive was trespassed upon by the other processor.
3     Ready            Drive is ready to accept commands.
4     Busy             Drive is executing a position command / reporting
                        an aborted seek / a trespass by the other processor.
5     Offset           Positioner is offset forward or reverse.
6     Write disable    Write circuits are disabled.
7     Drive ID         Identifies the selected drive as a model 6122.
8     Invalid address  Surface or cylinder capacity of the drive was exceeded.
9     Illegal command  Drive received an illegal read/write or position command.
10    Power fault      Power supply malfunction.
11    Pack unsafe      A condition imperiled the heads and pack.
12    Positioner fault Head positioner malfunction.
13    Clock fault      Servo clock malfunction.
14    Write fault      Write or head-select circuits malfunctioned.
15    Drive fault      Any of bits 8-14.
```

### `S`/`C`/`P` device-flag pulses and `IORST` (p.4, "S, C, P and IORST Functions")

```
f=S     Sets Busy=1, Done=0. Starts: READ, WRITE, FORMAT, READ BUFFERS, VERIFY.
f=C     Sets Busy=0, Done=0. Stops all data-transfer operations.
f=P     Starts: SEEK, RECALIBRATE, OFFSET, STOP, WRITE DISABLE, RELEASE,
        TRESPASS. Does NOT affect the Busy flag or Done flag.
IORST   Same as f=C, plus initiates a recalibrate on the lowest-numbered
        ready drive if not reserved by the other processor. Clears
        sector/surface address. Stored command defaults to Read.
```

The `f=P` "does not affect Busy/Done" line matters directly for the
driver: there is no software-visible completion flag for a seek pulse.
The manual resolves this not by having software poll for it, but by
documenting (p.12, Phase IV) that the *controller itself* holds off
transmitting a subsequently-issued read/write command to the drive
until that drive's outstanding seek finishes — see "Driver skeleton"
below for how that's used.

## The manual's own worked programming sequence (`pgmref.pdf` pp.11-12)

This is the single most load-bearing citation in this file — the
"worked examples" the task asked to use as a correctness check, since
there's no simulator to run the driver against instead. Five phases,
quoted/paraphrased directly:

1. **Select a drive and specify a seek command.** Issue `DIA` (request
   command channel), check Control Full is 0. Issue `DOA` specifying a
   seek command (reserves the drive). Issue `DIB`, check Ready.
2. **Position the heads.** Issue `DOC` (Specify Cylinder) **plus a `P`
   device flag command**. "The `P` command initiates the seek operation
   by setting the control full flag; it does not affect the
   controller's Busy or Done flags."
3. **Select a drive and specify a read/write command.** Same shape as
   phase 1, but with a `READ`/`WRITE`/etc. command instead of `SEEK`.
4. **Read or write.** Issue the first `DOC` (extended sector/count),
   then the second `DOC` (surface/sector/count), then `DOB` (memory
   address) **plus an `S` device flag command**. Quoting directly,
   because it's the key fact that removes any need to poll for seek
   completion in software: *"When the selected drive completes the
   previous seek operation and clears the associated seek busy flag,
   the controller transmits the stored read/write command to the
   adapter and the operation begins."*
5. **Release the drive** (dual-processor only — the manual explicitly
   says, in its own `NOTE` at the top of this section, p.11: *"In a
   single processor subsystem, ignore the release and trespass
   commands and also the trespassed flag, the drive reserved flag and
   the invalid status flag."* This driver follows that instruction
   literally — see "Scope" below).

## Driver skeleton: `examples/vulcan.h` / `examples/vulcan.c`

`vulcan_read_block(unit, blockno, buf)` / `vulcan_write_block(unit,
blockno, buf)`, transferring exactly one 256-word sector, modeled
directly on the five-phase sequence above (phases 1-4; phase 5 skipped
per the manual's own single-processor guidance):

- Converts a flat `blockno` (0 to 541,974) to cylinder/surface/sector
  via `%`/`/` against `DSKP_SECTORS_PER_SURF`(35)/`DSKP_SURFACES_PER_CYL`
  (19), matching the auto-increment order the manual itself describes
  (p.4: sector/count increment mid-transfer, surface increments after
  a track's last sector, matching "sector fastest, then surface, then
  cylinder").
- Phase 1/3's `DOA` and phase 4's two `DOC`s are issued **bare** (no
  pulse) — deliberately mirroring `examples/disk_probe.s`'s own
  reasoning (see `STORAGE_NOTES.md`): the `S`/`C`/`P` pulses are
  documented as device-level flags, not tied to whichever specific
  `DOA`/`DOB`/`DOC` instruction happens to carry them, so a stray pulse
  on the wrong register-load risks starting the *previous* stored
  command instead of the one just being written. Phase 2's seek and
  phase 4's transfer trigger are each issued as a single **combined**
  register-load-plus-pulse instruction (`DOCP`, `DOBS`) on the *last*
  register write of that phase, matching the manual's own phrasing
  ("issue a ... instruction (DOC) plus a P device flag command").
  `DOAS`/`DOBS`/`DOCS`/`DIAC`/`DIBC`/`DICC` combined forms are already
  proven against the real `dgasm` grammar (`eclipse_io.h`'s
  `outa`/`outb`/`outc`/`ina`/`inb`/`inc` macros use exactly these); the
  `P`-suffixed forms (`DOCP` here) were **not** previously exercised by
  any file in this repo — flagged as a real, if low-risk, gap (the Nova
  I/O instruction format's pulse field is documented as a uniform
  2-bit `S`/`C`/`P`/none selector shared across `DOA`/`DOB`/`DOC`/
  `DIA`/`DIB`/`DIC` alike, so there's no structural reason `P` would be
  accepted differently from the already-proven `S`/`C` — but "no
  structural reason" is not the same as "confirmed").
- No manual poll for seek completion between phase 2 and phase 3/4 —
  relying directly on the manual's own quoted guarantee above that the
  controller defers the transfer internally.
- Completion: `DOBS` (start) followed by a `SKPDN`-polling wait loop,
  structurally identical to `eclipse_io.h`'s `outb()` macro (in fact
  `outb(027, word)` would generate the same code — written out by hand
  here so the phase-specific commentary stays attached to it).
- Status check after completion: reads `DIA`'s bit 15 (R/W fault) and
  `DIB`'s bit 15 (Drive fault) — a coarse OR of each register's single
  summary bit, not a decoded per-condition error (see `vulcan.h`'s
  return-value documentation for exactly what a fuller version would
  need to expose instead).
- Extended/BMC-mapped addressing is **not used** — `buf` must be an
  ordinary pointer in this backend's normal 0-32767 logical range
  (`DOA`'s EMA-MSB field and `DOB`'s EMA-LSB bit are always left 0),
  the same restriction `mmpu.h`'s Phase 1 API and `disk_probe.s` both
  already operate under.

## What was and wasn't checked

**Checked — compiles, and compiles to the intended instruction
sequence**, via this project's real toolchain (`eclipse-cc`, i.e. the
actual `clang -cc1` → `opt` → `llc` → `reorder_asm.py` → `dgasm`
pipeline, no shortcuts):

```
$ eclipse-cc -o vulcan_test.simh vulcan.c vulcan_compile_test.c
eclipse-cc: retrying with __umodsi3 protected (needed by this program)
eclipse-cc: retrying with __udivsi3 protected (needed by this program)
[... other soft-float/conversion libcalls eclipse-cc's own retry loop
     needed for the % and / operators against `unsigned long blockno` ...]
eclipse-cc: wrote vulcan_test.simh
```

Zero warnings, zero errors, real `.simh` output produced (a small test
harness, `vulcan_compile_test.c`, calling both `vulcan_write_block` and
`vulcan_read_block` once each — not committed, scratch-only). This
confirms 32-bit `%`/`/` against `blockno` (needed for the cylinder/
surface/sector decomposition) resolves correctly against this target's
`__umodsi3`/`__udivsi3` (the same runtime routines `SOFT_FLOAT_NOTES.md`
already documents), and that dgasm accepts every instruction form used
(`DOA`, `DOB`, `DOC`, `DIA`, `DIB`, `DOCP`, `DOBS`, `SKPDN`) against
device `027` with no "unrecognised instruction" or "undefined symbol"
errors.

Beyond that, the generated assembly was inspected directly (`llc`'s
`-filetype=asm` output, before `dgasm` turns it into a `.simh` image)
to confirm the actual instruction *sequence* the compiler produced
matches the intended phase order, for both functions:

```
DIA 0,027        <- Phase 1: check Control Full
DOA 1,027        <- Phase 1: DOA specifying SEEK
DIB 1,027        <- Phase 1: check Ready
DOCP 1,027        <- Phase 2: Specify Cylinder + P pulse
DIA 0,027        <- Phase 3: check Control Full
DOA 1,027        <- Phase 3: DOA specifying READ/WRITE
DOC 0,027        <- Phase 4: first DOC (extended sector/count)
DOC 1,027        <- Phase 4: second DOC (surface/sector/count)
DOBS 1,027        <- Phase 4: Specify Memory Address + S pulse (start)
SKPDN 027
JMP vulcan_rw_wait<N>
DIA 0,027        <- status check
DIB 1,027        <- status check
```

identical for `vulcan_read_block` and `vulcan_write_block` apart from
the command value baked into the second `DOA`. This is real, if
narrow, evidence: it confirms the C source's control flow survived
compilation intact and the device code (`027`) is correct in every
generated instruction — it does **not** confirm any bit pattern's
actual on-the-wire correctness, timing, or that the drive would accept
any of this.

**Not checked, and explicitly not claimed**:

- **No execution, on any simulator or real hardware.** Running this
  against `eclipseemu` would not produce meaningful evidence even if
  attempted — the simulator doesn't model device `027` as anything at
  all, so `DIA`/`DIB` reads would return whatever an unimplemented
  device floats to (likely 0, but not confirmed), and — more
  seriously — `SKPDN`'s wait loop would very likely spin forever
  (`Done` never sets on a device the simulator never implements the
  Busy/Done handshake for), producing a hang, not a result. Running
  it anyway and reporting "it didn't crash" would be worse than not
  running it — actively misleading rather than merely absent evidence.
  This is why it wasn't attempted.
- **The two's-complement sector-count encoding** (`(unsigned int)(-1)
  & 0x3F` for a 1-sector transfer) is derived from the manual's own
  wording ("two's complement of the number of sectors") by direct
  analogy with the standard DG negative-down-counter convention used
  elsewhere in this architecture family — not verified against a
  worked numeric example, because the Programmer's Reference does not
  actually give one for this field (checked: pp.6-7, the only content
  there is the bit-table captions quoted above, no example count value
  is shown for any specific sector count).
- **Fault recovery / ECC correction** (`pgmref.pdf` pp.22-23's
  generator-polynomial-based correction algorithm) is entirely out of
  scope here, as instructed — this driver detects a fault (returns
  `-3`) but does not attempt correction.
- **Interrupt-driven completion** is not implemented — polled only,
  same simplification `STORAGE_NOTES.md` made for `DSK`.
- **Multi-sector transfers** are not implemented — one sector (256
  words) per call, matching `disk_probe.s`'s own scope for `DSK`.
- **The `DOCP`/`P`-pulse combined-instruction form** — see "Driver
  skeleton" above; accepted by `dgasm` (no assembly-time error), but
  its *hardware* behavior (does the real controller actually latch the
  cylinder register from the same instruction word that carries the
  `P` pulse, versus needing the register load to have landed on a
  prior, separate instruction?) is not something dgasm accepting the
  syntax can confirm either way.

## What real-hardware testing would still be needed

Precisely, so this isn't hand-waved: before trusting this against the
user's real drive,

1. **A serial connection to the real machine** (per this project's
   `README.md`, `eclipse-run.sh`) with a real Vulcan/6122 unit attached
   and a scratch cylinder/surface/sector safe to overwrite.
2. **Single-step or trace confirmation of the Phase 1-4 sequence**
   against the real controller's status registers — at minimum, read
   `DIB` after the Phase 1 `DOA` and confirm `Ready` actually reads as
   expected for the target drive, before ever reaching a pulse that
   moves heads or touches data.
3. **A single write-then-read round trip** on one known-scratch sector,
   the same shape as `disk_probe.s`'s own proof for `DSK` — write a
   marked pattern (first word, second word, and *last* word of the
   256-word sector, to prove the whole sector moved), then read it back
   into a separate buffer and diff.
4. **Confirming the `DOCP` combined form actually works** as a single
   instruction on real hardware, or falling back to a bare `DOC`
   followed by a separate `NIOP` pulse-only instruction if it doesn't
   (both are valid dgasm syntax; only real hardware settles which one
   the real 6122 controller actually wants).
5. **A deliberate fault-path test** — e.g., an out-of-range
   cylinder/sector, or a write-disabled drive — to confirm the status
   bits this driver checks (`DIA` bit 15, `DIB` bit 15) actually set the
   way the manual describes, and that the coarse pass/fail this driver
   returns doesn't mask a real failure some other way.

None of these five have been done. Everything above this section is a
manual-faithful design, checked for internal consistency and checked to
compile to the intended instruction sequence — not a device that has
been proven to work.

## Regression check

Only new files were added (`examples/vulcan.h`, `examples/vulcan.c`,
this document) — no existing file was modified, so existing baselines
are unaffected by construction. `examples/kismet.h`/`kismet.c` (seen as
untracked in `git status` alongside this work) belong to a different
parallel agent and were not read or touched.
