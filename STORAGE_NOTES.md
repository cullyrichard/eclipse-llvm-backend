# Storage device investigation — scoping a first OS-level block driver

Prompted by a real question in service of a larger OS-scoping effort:
before writing a block-storage driver and filesystem for this backend,
which of the two disk devices this SIMH build actually models (`DSK`,
`DKP`) should the first driver target, what's their real hardware
identity and capacity, and can a program *running on the simulated
Eclipse CPU* (not SIMH's own `attach`/`examine` commands manipulating
the disk image file directly) actually drive one end-to-end — a single
raw block write followed by a readback, proving real data round-trips
through the device.

This is a research + minimal-probe task, not a full driver build. The
goal is a solid, evidence-backed foundation the next increment (a real
block driver + filesystem) can build on directly.

## What's actually in this SIMH build

`show devices` against the real `~/dev/simh-src/BIN/eclipse` binary:

```
DSK
DKP     4 units
MTA     8 units
```

(`MTA` is magtape, out of scope — this project already has a separate,
mature tape toolchain in `eclipse-toolchain/`.)

`show dkp` / `show dsk`:

```
DKP     4 units
  DKP0  1247KW, not attached, write enabled
        autosize
  DKP1  1247KW, not attached, write enabled
        autosize
  DKP2  1247KW, not attached, write enabled
        autosize
  DKP3  1247KW, not attached, write enabled
        autosize
DSK
        262KW, not attached
```

## Real hardware identity

**`DSK` — device code `020` octal (16 decimal), interrupt bit `PI_DSK =
0000100`.** `~/dev/simh-src/NOVA/nova_dsk.c`'s own header comment states
this directly, not inferred: `nova_dsk.c: 4019 fixed head disk
simulator` / `dsk  fixed head disk` / `The 4019 is a head-per-track
disk. To minimize overhead, the entire disk is buffered in memory.`
This is the DG **4019** fixed-head (head-per-track) disk. I could not
find a scanned DG manual for the 4019 specifically on bitsavers (the
`bitsavers.org/pdf/dg/disc/` index has `4046_4047_4049/`,
`10mb_disk/`, `10_MB_Disk_Controller/`, and `Disc_Subsystem/`, but
nothing named for the 4019) — per this project's own stated fallback
rule, the identification here rests on the SIMH source comment alone,
clearly flagged as such rather than presented as manual-verified.

**`DKP` — device code `033` octal (27 decimal), interrupt bit `PI_DKP =
0000400`.** `nova_dkp.c`'s header: `nova_dkp.c: NOVA moving head disk
simulator` / `dkp  moving head disk`. This is a general-purpose
moving-head disk *pack controller* that can drive 11 different real DG
drive types (`drv_tab[]` in `nova_dkp.c`, each with its own real DG or
DG-OEM part number in the `dkp_mod[]` modifier table: `6030` floppy,
`6097` DS/DD floppy, **`4047` (Diablo 31)**, `4234`/`6045` (Diablo 44),
`4048`/`C111`, `2314`/`4057`/`C114`, `6225`, `6227`, `6099`, `6103`,
`4231`/`3330`). The unit default (what `show dkp` reports, `1247KW`) is
`TYPE_D31`, explicitly labeled `"4047 (Diablo 31)"` in the source — a
real DG cartridge-disk drive, OEM'd from Diablo Systems' Model 31. A
real scanned DG manual for this family **was** found: bitsavers hosts
`015-000005-02_4046_4047_4049_Technical_Manual` (the 4046 is DG's
moving-head disk controller, 4047/4049 the drive units it drives) at
`https://bitsavers.org/pdf/dg/disc/4046_4047_4049/015-000005-02_4046_4047_4049_Technical_Manual_107305.pdf`,
plus an illustrated parts list at the same path
(`016-000018-00_4046_Volume_II_Illustrated_Parts_List_197208.pdf`) —
this confirms 4047/Diablo-31 is real, historically-shipped DG hardware
and not a guess, though the manual is a large scanned PDF and its
register-level content was **not** read page-by-page here (out of
scope for this probe — worth doing before a full `DKP` driver is
written; the register/command layout used below comes from
`nova_dkp.c` directly, the same fallback rule as `DSK`, clearly flagged).

Real capacity, computed from `nova_dkp.c`'s own geometry table and
cross-checked against `show dkp`'s own report: Diablo 31 = 12
sectors/surface × 2 surfaces/cylinder × 203 cylinders × 256
words/sector = 1,247,232 words ≈ 1247KW per unit, **× 4 units** (`DKP0`-
`DKP3`, each independently attachable, independently sized —
`DKP_NUMDR = 4`). `DSK`'s default (1 platter) = 128 tracks × 8
sectors/track × 256 words/sector = 262,144 words ≈ 262KW, expandable to
8 platters (`UNIT_PLAT`, "1P".."8P") on the same single unit — `DSK`
is a single-unit device, not 4 independent drives like `DKP`.

## I/O programming model (verified directly against source, both devices)

Both devices use the **same generic Nova/Eclipse device convention**
this project's `eclipse_io.h` already assumes for TTI/TTO (Busy/Done
flip-flops, `DEV_SET_BUSY`/`DEV_SET_DONE`/`DEV_UPDATE_INTR`,
`SKPBN`/`SKPDN` polling, `INT_REQ` wired for interrupt-driven
completion too) — unlike the FPS-100 coprocessor's ad hoc two-register
protocol (`examples/fps.h`'s `fpu_out`/`fpu_in`), which this project
already documented as *not* using this handshake at all. Both disk
devices also transfer data by **simulated cycle-stealing DMA**: once
triggered, the device's own service routine (`dkp_svc`/`dsk_svc`)
copies an entire sector directly between the file buffer and `M[]`
(Eclipse main memory) via `MapAddr()` — the same data-channel address
translation the MMPU investigation's `mmpu_probe.s` work already
covers (`MMPU_NOTES.md`) — with **no further CPU involvement per word**.
This is real Nova/Eclipse "data channel" I/O, not programmed I/O like
`fpu_out`/`fpu_in`'s per-word `DOA`/`DOB`.

### `DSK` register layout (`nova_dsk.c`)

| Instruction | Effect |
|---|---|
| `DOA <AC>, DSK` (no pulse) | sets `dsk_da` — linear block address. Block×256 words must stay under the unit's word capacity (1024 blocks for the default 262KW/1-platter size). |
| `DIA <AC>, DSK` (no pulse) | reads `dsk_stat & DSKS_ALLERR` (write-lock / data-late / nonexistent-disk / CRC / error-summary bits). `0` = no error. Does **not** clear status (only a pulsed form does). |
| `DOB <AC>, DSK` (no pulse) | sets `dsk_ma` — starting memory address for the transfer. |
| `DIB <AC>, DSK` | reads `dsk_ma` back. |
| `DIC <AC>, DSK` | "undocumented DG feature" per the source's own comment — always returns `256` (the fixed sector size in words). |
| `NIOS DSK` | pulse S: starts a **read** (disk block → memory, `DSK_NUMWD` = 256 words). |
| `NIOP DSK` | pulse P: starts a **write** (memory → disk block). |
| `NIOC DSK` / any `C`-pulsed form | clears busy/done, resets `dsk_stat`, cancels any pending transfer. |
| `SKPDN DSK` / `SKPBN DSK` | skip-next on Done / Busy — standard polled-completion idiom. |

No cylinder/head/sector geometry is exposed to software at all — `DSK`
presents a **flat linear block address space** (0 to `NUMDK×NUMTR×NUMSC
- 1`); the device's internal `sector_map[]` interleave table and
rotational-position timing (`GET_SECTOR`) are entirely hidden from the
program. This maps almost directly onto a `read_block(dev, blockno,
buf)` abstraction with zero translation logic needed in software.

### `DKP` register layout (`nova_dkp.c`)

| Instruction | Effect |
|---|---|
| `DOC <AC>, DKP` (no pulse) | sets `dkp_ussc` — packed unit(2b)/surface(5b)/sector(5b)/count(4b) register (`newf`-format drives; older drives use a 6b/4b surface/sector split — decoded per-drive-type by `drv_tab[dtype].newf`). |
| `DIC <AC>, DKP` | reads `dkp_ussc` back. |
| `DOA <AC>, DKP` (no pulse) | sets `dkp_fccy` — packed flags(5b)/command(2b)/cylinder(9b) register. Command = `READ`(0)/`WRITE`(1)/`SEEK`(2)/`RECAL`(3). Bit 15 set also clears prior error flags. |
| `DIA <AC>, DKP` | reads `dkp_sta` — drive-ready, seeking/seek-done per-unit bits, cross-cylinder/bad-cylinder/unsafe/CRC/data-late/error/done bits. |
| `DOB <AC>, DKP` (no pulse) | sets `dkp_ma` (memory address) + data-channel map select (bit 15). |
| `DIB <AC>, DKP` | reads `dkp_ma` back. |
| `NIOS DKP` (or any `S`-pulsed form) | starts a read/write **only** if the command register says `READ`/`WRITE` — starting a `SEEK`/`RECAL` this way is rejected. |
| `NIOP DKP` (or any `P`-pulsed form) | starts a **seek/recal** — the *only* pulse that starts those; also used for an undocumented DG diagnostic-sizing "crock" the source calls out by name. |
| `SKPDN DKP` / `SKPBN DKP` | standard polled-completion idiom, same as `DSK`. |

Unlike `DSK`, a real transfer needs the driver to compute
cylinder/surface/sector from a linear block number itself (`GET_SA`'s
`(cyl×surf_per_cyl + surf)×sect_per_cyl + sect` formula, mirrored in
software), issue a `SEEK` first (`P`-pulsed), poll seek-done, *then*
issue the actual `READ`/`WRITE` (`S`-pulsed) — a real state machine,
not a single register-and-go operation. This is standard for a real
moving-head disk, but it is meaningfully more software than `DSK`
needs for the same read/write outcome.

## Device choice: `DSK`, not `DKP`, for the first probe/driver

The premise going in (per this investigation's own brief) was that
`DKP`'s "4 units, moving-head disk pack" framing sounds more
Unix-filesystem-like than `DSK`. That intuition doesn't survive contact
with the actual register model, checked above rather than assumed:

- **`DSK` already presents a flat block address space** — exactly the
  `read_block(dev, blockno, buf)` shape a filesystem driver wants, with
  the device itself (not the driver) handling interleave and
  rotational timing. `DKP` requires the driver to implement seek
  sequencing and cylinder/surface/sector translation *before* it can
  offer that same abstraction — real, useful work, but a second
  increment's worth, not a first probe's.
- **`DSK` is a single register-and-go operation per transfer**: set
  block address, set memory address, pulse, poll, done. `DKP` is a
  multi-step state machine (seek, poll seek-done, then read/write,
  poll again) with more failure modes (bad cylinder, cross-cylinder,
  seek errors) to handle correctly even for a minimal success path.
- Both are equally real, bit-accurate simulated DG hardware — this
  isn't "DSK is a toy and DKP is the real device." `DSK` genuinely
  models the DG 4019. Its capacity (262KW/platter, up to ~2MW across 8
  platters on the one unit) is smaller than `DKP`'s 1247KW × 4 = ~5MW
  aggregate, but plenty for a first filesystem to develop against.

**Call: build the first probe — and the first real block-driver
increment — against `DSK`.** `DKP`'s richer, more realistic
seek/geometry model is the natural *second* step once the basic
plumbing (attach, register discipline, completion handling, a real
verified round trip) exists and is trusted; its register set is a
strict superset of what `DSK` already requires (it adds seek and
cyl/surf/sect, on top of the same address/mem-address/pulse/poll shape
`DSK` uses). Revisit this file when that happens.

## Empirical verification: real read/write round trip via `DSK`

`examples/disk_probe.s` (new, committed): entirely a program running on
the simulated Eclipse CPU. No SIMH `attach`/`examine`/`deposit` command
ever touches the payload data directly — `attach` only opens the
(initially-blank, auto-created) image file; every register write,
transfer trigger, completion poll, and status/data readback happens via
`DOA`/`DOB`/`DIA`/`NIOP`/`NIOS`/`SKPDN` instructions the assembled
program itself executes.

Sequence:
1. Write phase: `DOA`s block number `5` into `dsk_da`, `ELEF`s the
   address of a 256-word buffer `wbuf` (word 0 = `0123456` octal, word
   1 = `0000377` octal, word 255 = `0177777` octal, words 2-254 zero)
   into `dsk_ma` via `DOB`, pulses `NIOP DSK` to start the write, polls
   `SKPDN DSK` to completion, reads status via `DIA` into `wstatus`.
2. Read phase: re-sets `dsk_da` to block `5` (a fresh, explicit
   register set, not relying on any leftover state), points `dsk_ma` at
   a *separate*, pre-zeroed 256-word buffer `rbuf` via `DOB`, pulses
   `NIOS DSK` to start the read, polls to completion, reads status into
   `rstatus`.
3. `HALT`.

Assembled and run against the real `eclipse` SIMH binary (no LLVM/
backend involvement — hand-assembled, same methodology as the MMPU
probes):

```
$ dgasm -t eclipse_s140 -f simh -o disk_probe.simh disk_probe.s
$ rm -f disk_probe.img
$ { echo 'attach dsk disk_probe.img'; cat disk_probe.simh; \
    echo 'dep PC 50'; echo 'run'; \
    echo 'e PC'; \
    echo 'e 1101'; echo 'e 1102'; \
    echo 'e 501'; echo 'e 502'; echo 'e 1100'; \
    echo 'e 101'; echo 'e 102'; echo 'e 500'; \
    echo 'quit'; } | eclipse

%SIM-INFO: DSK: creating new file
%SIM-INFO: DSK: buffering file in memory

HALT instruction, PC: 00077 (JMP 0)
PC:	00077
1101:	000000        <- wstatus: 0 = no error on the write
1102:	000000        <- rstatus: 0 = no error on the read
501:	123456         <- rbuf[0]:   matches wbuf[0]  (0123456)
502:	000377         <- rbuf[1]:   matches wbuf[1]  (0000377)
1100:	177777         <- rbuf[255]: matches wbuf[255] (0177777)
101:	123456         <- wbuf[0] unchanged (source buffer intact)
102:	000377         <- wbuf[1] unchanged
500:	177777         <- wbuf[255] unchanged
%SIM-INFO: DSK: writing buffer to file: ./disk_probe.img
```

`wstatus`/`rstatus` both `0` (no error flags), and all three marker
words (first, second, and *last* word of the 256-word sector — proving
the whole sector moved, not just its first words) came back identical
in a separate buffer the read phase started at zero. This is the core
claim, verified directly: **a program running on the simulated Eclipse
CPU can write a real block to the `DSK` device and read it back
through a completely separate memory buffer, byte-for-byte correct.**

**Independent second check**, outside the simulator entirely — read the
raw backing file `disk_probe.img` at the exact byte offset block 5
should live at (`5 × 256 words × 2 bytes/word = 2560`):

```
$ python3 -c "
import struct
with open('disk_probe.img','rb') as f:
    data = f.read()
off = 5*256*2
w = struct.unpack_from('<256H', data, off)
print(oct(w[0]), oct(w[1]), oct(w[255]))
"
0o123456 0o377 0o177777
```

Confirms the write genuinely reached the correct offset in the real
backing file — not just an artifact of SIMH's in-memory buffering
being read back by the same process. (Note: `UNIT_MUSTBUF` means `DSK`
buffers the whole unit in host memory while attached and only flushes
to the file on detach/`quit` — `"DSK: writing buffer to file"` above is
that flush; the file is genuinely a 512-byte-aligned image of the
attached "disk" from that point on, sized to the highest block ever
touched, not pre-allocated to full capacity.)

## What a minimal block-driver API would still need

`read_block(dev, blockno, buf)` / `write_block(dev, blockno, buf)`
against `DSK` is now close: this probe proves the exact register
sequence works from CPU-executed code. What's still missing to turn it
into a real driver, honestly:

- **A C-callable wrapper**, not hand assembly. `dsk_da`/`dsk_ma` are
  16-bit registers reached via bare `DOA`/`DOB` — should translate
  directly to `eclipse_io.h`-style inline-asm macros (`outa`/`outb`
  don't fit as-is, see below), callable with a real `blockno`/`buf`
  pointer from C.
- **`outa`/`outb`/`ina` from `eclipse_io.h` don't fit `DSK` directly.**
  Those macros always issue the `S`-pulsed form (`DOAS`/`DOBS`) so a
  single instruction both sets the register *and* pulses — fine for a
  device where pulsing has no side effect beyond Busy/Done, but on
  `DSK` an `S` or `P` pulse on *any* register-setting instruction
  triggers a transfer (`if (pulse) {...} if (pulse & 1) {transfer}` in
  `nova_dsk.c` runs regardless of which register (`code`) the pulse
  rode in on). `disk_probe.s` avoids this by issuing bare, pulse-less
  `DOA`/`DOB` (same convention `examples/fps.c`'s `fpu_out` already
  uses, for an unrelated reason) and triggering the transfer
  separately via bare `NIOS`/`NIOP`. A real driver needs its own
  small `dsk_out`/`dsk_trigger`-style macro pair, not the generic ones.
- **Interrupt-driven completion is unexercised.** This probe polls
  (`SKPDN`). `INT_DSK`/`PI_DSK` are wired through the same
  `DEV_UPDATE_INTR` path every other device here uses, so a real
  driver could take a completion interrupt instead of spinning — worth
  doing once there's an interrupt-driven scheduler to hand control
  back to, not before.
- **Error handling is unexercised.** This probe's status came back `0`
  both times (clean path only). `DSKS_WLS`/`DSKS_DLT`/`DSKS_NSD`/
  `DSKS_CRC` are real, distinct failure modes (`nova_dsk.c`) a real
  driver needs to detect and propagate — not attempted here.
- **No DMA *setup* is needed for `DSK`** (unlike, say, the FPS-100's
  host-DMA path in `examples/fps_dma_test2.c`) — the "data channel"
  transfer here is entirely internal to the simulated device once
  triggered; software only supplies the two addresses and a pulse.
  Nothing is blocked on DMA configuration for this device.
- **Multi-sector transfers, and `DKP`'s geometry translation**, are the
  natural next increment once `DSK`'s basic plumbing above is real
  C code instead of hand assembly.

## Regression check

Only new files were added (`examples/disk_probe.s`, this document) —
no existing file was modified, so existing baselines are unaffected by
construction.
