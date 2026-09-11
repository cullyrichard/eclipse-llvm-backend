# Argus (DG Model 6236/6237 DG-Disk Subsystem) — scoping notes

**Bottom line up front: the only Argus document in the archive is an
Installation Data Sheet (mechanical/electrical/cabling drawings), not
a Programmer's Reference. It genuinely does not contain a DIA/DIB/
DIC/DOA/DOB/DOC-style register map, command set, status-bit layout,
capacity, or geometry. Per this project's own no-fabrication rule, no
driver skeleton was written this round — see "Why no code" below. A
real handful of facts *were* extracted and are cited below (device
select code, DMA-class architecture, confirmed S/140 cabling support),
which narrows what a future register-level source would need to
supply.**

## What was checked

Per the task's own guidance, the direct archive URL given
(`.../010-000355-00_IDS_..._Preliminary__1983.c.pdf`, single
underscore before `IDS`) 404s. The working link (double underscore)
was found by browsing the live index at
`novasareforever.org/archives/documentation/dg.hw/dg_hw_disks`:

```
010-000355-00__IDS_DG-Disk_Subsystem_Model_6236_ARGUS_Preliminary__1983.c.pdf
```

Confirmed by re-browsing the same index page a second time, filtering
specifically for "6236"/"Argus": **this is the only Argus-related
document in the archive** — no non-preliminary/final version exists to
substitute in. Downloaded and read as actual page images (`pdftoppm`
at 400 DPI, rotated to reading orientation, `pdfinfo`/`Read` — not
WebFetch's lossy text conversion), all 11 pages. `pdfinfo` metadata:

```
Title:   010-000355-00 IDS DG-Disk Subsystem Model 6236 ARGUS Preliminary 1983
Author:  Wild Hare Computer Systems Inc. - Bruce K. Ray
Pages:   11
```

Every one of the 11 sheets carries a hand-stamped **"PRELIMINARY"**
box in the lower-right, and the title block on every sheet reads
`TITLE: INSTALLATION DATA SHEET / DG/DISK SUBSYSTEM / MODEL 6236/6237`,
`CODE IDENT 34984` (Data General's CAGE code), drawing number
`000355`. The `010-` archive filename prefix matches this project's
own established convention (see `STORAGE_NOTES.md`'s citation of the
same archive's classification scheme) for "installation data sheets
and configuration drawings" — and the actual content matches that
classification exactly: unpacking/rack-mounting procedures, cabinet
dimensions, power/environmental specs, cabling tables, and PCB
*jumper* settings. There is no register bit-layout table, no op-code
list, no status-word description, and no capacity or geometry figure
anywhere in the 11 sheets — this was read in full, not sampled.

## What the document actually says

### Physical / mechanical / power (sheet 1 of 11, "Installation Specifications, Model 6236")

- Dimensions: 482.8 × 759 × 267 mm (19 × 29.87 × 10.5 in).
- Weight: 58.5 kg / 130 lb.
- Power: ~600 W typical (100/120/220/240 V variants, 50/60 Hz),
  +5 V draw for the CPU-resident controller PCB specifically called
  out as **13.5 A** (see below).
- Operating temp 10–38°C (50–100°F); storage −40 to 65°C (−40 to
  149°F).
- **Major component table** (sheet 1): item **A** = "Rigid Disk
  Drive" (cabinet-mounted) — the word "Rigid" (vs. the fixed-head
  4019/`DSK` this project already documented in `STORAGE_NOTES.md`)
  is itself informative; item **H** = "Controller PCB **005-14278**",
  mounting location **CPU** (not the disk cabinet) — i.e. the host
  interface card physically plugs into the CPU backplane, not the
  Argus cabinet. This matches the BMC/data-channel-class architecture
  below.
- No capacity (MB/KW), no cylinder/head/sector count, no transfer
  rate is stated anywhere in the document. This is a real, confirmed
  gap, not an oversight in reading it.

### CPU compatibility and cabling (sheets 10–11, "System Interconnections", Table No. 1)

Table No. 1 ("CPU Internal & Interface Cables") lists, by exact CPU
designator/type, the specific internal and external cable part
numbers required — and **explicitly includes the S/140**:

```
CPU DESIGNATOR   CPU TYPE   CPU INTERNAL CABLE   10'        20'        30'        40'
13-14            M600       005-020216            005-020298 005-020631 005-020632 005-020633
13-14            S250       005-020216            005-020298 005-020631 005-020632 005-020633
13-14            S350       005-020216            005-020298 005-020631 005-020632 005-020633
13-14            MV8000     005-020216            005-020298 005-020631 005-020632 005-020633
22-22            MV6000     005-020216            005-020298 005-020631 005-020632 005-020633
22-22            S140       005-020216            005-020298 005-020631 005-020632 005-020633
70-89            S280       005-020104            005-018480 005-021154 005-020629 005-020630
70-89            MV4000     005-020104            005-018480 005-021154 005-020629 005-020630
70-89            MV10000    005-020104            005-018480 005-021154 005-020629 005-020630
70-89            MV/8000-II 005-020104            005-018480 005-021154 005-020629 005-020630
```

This table is identical (down to the `S140` row) on both sheet 10
(Model 6236) and sheet 11 (Model 6237), confirming the real S/140 was
an officially cabled, supported host for this exact subsystem — not
a guess. "CPU designator 22-22" groups S/140 with MV/6000 for cabling
purposes (backplane slot/connector class), which is a separate axis
from the I/O device select code below.

Daisy-chain notes on the same sheets: up to 4 drives per controller
port ("MAX CONFIGURATION PER CONTROLLER SHOWN" with 4 drives drawn),
dual-port drives follow the same rule as single-port ("DUAL PORT SAME
RULES AS SINGLE PORT"), and "FOR FEWER THAN 4 DRIVES PER CONTROLLER,
ELIMINATE 6' DAISY-CHAIN CABLES AND LOCATE TERMINATOR IN TOP POSITION
OF THE TOP DRIVE."

### Controller board, DMA class, and device select code (sheet 6 of 11, "Tailoring / Jumpering")

This is the one sheet with anything I/O-register-adjacent. The board
is labeled:

```
HIGH DENSITY FILE INTERFACE PCB
CPU RESIDENT
Ref DGC Dwg No 003-001578 Rev 00
```

("CPU resident" confirms sheet 1's major-component table — this card
lives in the CPU backplane, matching item H's "CPU" mounting
location.) Two jumper tables are given:

**Table I — Bus Priority Jumpers** (labeled on the drawing as "BMC
PRIORITY JUMPERS SEE TABLE I" — i.e. this is explicitly a **Burst
Multiplexor Channel (BMC)** device, DG's cycle-stealing DMA-class I/O
channel, the same channel class this project's `MMPU_NOTES.md`
already distinguished from the core MMPU during its own investigation):

```
PRIORITY   INSTALLED JUMPERS
MSCR 7 (highest)  P17 P18 P19 P20
MSCR 6            P16 P18 P19
MSCR 5            P15 P18 P20
MSCR 4            P14 P18
MSCR 3            P13 P19 P20
MSCR 2            P12 P19
MSCR 1            P11 P20
MSCR 0 (lowest)   P10
```

**Table II — Device Code Select Jumpers** (jumper IN = 1, OUT = 0),
bits P1 (MSB) through P6 (LSB):

```
DEVICE CODE        P1  P2  P3  P4  P5  P6
PRIMARY   = 24 (8)   0   1   0   1   0   0
SECONDARY = 64 (8)   1   1   0   1   0   0

"OTHER DEVICE CODES ASSIGNED PER OCTAL ARRANGEMENT OF P1-P6."
```

Decoding: `010100` = 024 octal, `110100` = 064 octal — both match the
document's own printed labels, so this is read correctly. **This is a
genuine, confirmed fact: the Argus host interface's I/O device select
code is 024 octal for the primary controller, 064 octal for a second
(dual-controller) configuration**, with the general rule that other
codes are obtained by re-jumpering P1–P6 in the same binary/octal
pattern. This is exactly the kind of number a real driver needs (the
device-code operand every `DIA`/`DOB`/`NIOS`/`SKPDN`-style instruction
in `examples/disk_probe.s` takes) — but it is the *only* register-level
fact the document supplies. It says nothing about what `DIA`/`DIB`/
`DIC`/`DOA`/`DOB`/`DOC` individually mean for this device, what
command codes or status bits exist, or how a block address is loaded.

### Front panel and read/write board (sheet 7 of 11, "Tailoring (cont)")

Three more boards are named with their internal DG schematic numbers,
none of which are register maps, all of which are physically-toggled
switches, not software-visible registers as shown:

- **Control Panel** (`Ref DGC Dwg No 003-001825 Rev 01`): a
  technician diagnostic readout, switches SW1–SW5, function table:
  "DISPLAY UNIT NO. AND ERROR CODE / DISPLAY CURRENT CYLINDER / LOOP
  ON POWER FAIL TEST / LOOP ON RANDOM SEEKS", with "SELECT DISPLAY
  READOUT/FUNCTION PER SW5, SW3. SELECT UNIT NUMBER PER SW1, SW2." and
  the caveat "READ OUT FUNCTIONALITY OF SWITCHES OBSERVED WITH FRONT
  PANEL REMOVED" — i.e. this is bench/service tooling, not proven to
  be host-addressable.
  - **Architecturally useful anyway**: "DISPLAY CURRENT CYLINDER" and
    "LOOP ON RANDOM SEEKS" confirm Argus is a **moving-head,
    cylinder-addressed** disk (seeks are a real, timed operation with
    its own diagnostic loop mode) — a different device class from the
    fixed-head DG 4019 (`DSK`) this project already drove end-to-end
    in `STORAGE_NOTES.md`/`examples/disk_probe.s`. A real Argus driver
    would need seek-completion handling that `DSK` never required.
- **Read/Write board** (`Ref DGC Dwg No 003-001821 Rev 02`): one
  physical `WRITE PROTECT`/`WRITE ENABLE` toggle (SW1).
- **Power Amplifier** (`Ref DGC Dwg No 003-001827 Rev 01`): a
  "normal positioner operation" vs. "manual disable positioner"
  switch — head-positioner servo electronics, drive-internal.

None of sheets 2–5, 8, or 9 (installation specs for the companion
Model 6237 cabinet, rack-mounting hardware, shipping/unpacking
procedure, cabinet mounting brackets, AC voltage/frequency tailoring)
contain anything beyond mechanical/electrical installation detail —
read in full, nothing register-related there.

## Why no driver code was written this round

Per the task's own explicit instruction: writing `argus_read_block`/
`argus_write_block` (or any register-level probe in the
`examples/mmpu.c`/`examples/disk_probe.s` style) requires knowing, at
minimum, what each of `DIA`/`DIB`/`DIC`/`DOA`/`DOB`/`DOC` does for
this device — which register carries a command vs. status vs. a block
address vs. data, what the busy/done/error semantics are, and (since
this is confirmed BMC/DMA, not `DSK`-style simple programmed I/O) how
a BMC transfer is set up and triggered. **None of that is in this
document.** Inventing plausible-sounding values for any of it — even
staying "close" to the NOVA/Eclipse I/O instruction conventions this
project already knows well from `DSK`/MMPU work — would produce code
that looks verified but isn't, which is explicitly worse than an
honest gap here. So, unlike Zebra/Vulcan/Kismet (which per the task's
own framing have proper technical/programmer's-reference-class
manuals), Argus hits the real, hard information wall the task said to
expect as a live possibility. No `examples/argus*` files were created.

This also could not have been worked around by simulation the way
prior probes in this repo were: `~/dev/simh-src/NOVA/*.c` has no
"argus"/"6236" reference at all, confirmed by grep before starting —
so even a speculative register layout couldn't be single-step-verified
against a real emulation the way `disk_probe.s`/`mmpu_probe.s` were.
Both the documentation and the verification path are closed for now.

## What would actually unblock this

In order of likelihood of existing, most useful first:

1. **A DG Programmer's Reference / Technical Manual for the "High
   Density File Interface"** controller card or for Argus specifically
   — the natural place for a DIA/DIB/DIC/DOA/DOB/DOC register map to
   live. The schematic/logic-diagram numbers this Installation Data
   Sheet itself cites (`003-001578` for the CPU-resident interface
   PCB, `003-001821` for the Read/Write board, `003-001827` for the
   Power Amplifier, `003-001825` for the Control Panel) are real DG
   internal document numbers and a natural search target if any DG
   corporate archive, bitsavers-style scan collection, or the
   novasareforever archive itself is updated later — worth periodically
   re-checking `dg_hw_disks` (and, if it exists, an equivalent
   `dg.hw.disk-controllers`-style section) for a `003-` or `014-`
   prefixed document, since `014-` is the prefix this project's own
   MMPU work found for the real S/140 Programmer's Reference.
2. **Direct inspection of the user's real Argus unit.** The device
   select code (024/064 octal) and BMC-channel class narrow this
   usefully: on a real S/140 with the unit attached, a supervisor-mode
   probe modeled on `examples/disk_probe.s` (bare `DIA`/`DIB`/`DIC`
   reads at device code 024 with the drive in a known state — e.g.
   before/after a manual seek via the front panel's own diagnostic
   switches) could reverse-engineer status-bit meaning empirically,
   the same rigor-over-speculation approach this project's memory file
   already endorses ("heavy autonomous forking is fine if each
   increment shows real evidence"). This is real hardware work, not
   something this task should attempt blind — flagging it as the
   concrete next step if/when the user wants to spend hardware time
   on it, rather than attempting it here.
3. Barring either of those, Argus block-driver work stays blocked.
   The facts in this document (select code, BMC/DMA class, confirmed
   S/140 cabling support, moving-head/cylinder architecture) are real
   and worth keeping, but they are not sufficient to write or claim
   any verified I/O sequence.
