# MMPU (Memory Mapping and Protection Unit) — Phase 1 investigation notes

Prompted by a real question: every function/global this backend compiles
is addressed via `ELDA`/`ESTA`/`EJSR`/`ELEF`, whose absolute operand
`dgasm` hard-caps at 0-32767 (`opcode.c`'s `report_error("Address out of
range. Got %d, should be 0 - 32767", ...)`, confirmed fatal — `main.c`
checks `get_err_count()` and refuses to emit output). Does the real
Eclipse S/140 have a way to reach more physical memory than that 32768-word
ceiling? Yes — the MMPU — but it does *not* raise that ceiling; it
remaps which physical words a program's existing 32768-word logical
window lands on. This is Phase 1 only: the plumbing is real and
empirically verified, but nothing here is wired into the LLVM backend,
there's no C-callable API, no page-fault handling, and no OS. See
"What Phase 2 would actually need" at the end.

## What works

Verified today, entirely in supervisor mode, no LLVM backend changes,
**no `dgasm` changes** (see "A pleasant surprise" below): a single `ESTA`
can be redirected, for exactly one instruction, to a physical address
far past the 32768-word logical ceiling — see `examples/mmpu_probe.s`
and the transcript at the end of this section.

The mechanism (`SIMH`'s `NOVA/eclipse_cpu.c`, the emulation this
project's entire verification methodology already trusts — see
`README.md`'s own description of how rigorously the `-f simh` path was
checked before being relied on):

- **Logical addressing is unchanged.** Every logical address a program
  issues is still 0-32767 (`page = (addr>>10)&037`, `037` = 5 bits = 32
  logical pages; `addr & 001777`, `001777` = 10 bits = 1024-word
  in-page offset; 32 × 1024 = 32768). The MMPU does not give a program a
  wider address to compute with — it changes what physical word a given
  logical address means, page by page.
- **Physical address = `((Map[ctx][page] & PAGEMASK) << 10) | (addr &
  001777)`** (`GetMap`/`PutMap`, `eclipse_cpu.c` ~5510-5610). `PAGEMASK
  = 01777` = 10 bits = physical page numbers 0-1023. Physical page ×
  1024-word pages = **1,048,576 words (1 megaword) maximum** — this is
  hard-coded (`nova_defs.h:77`, inside `#if defined(ECLIPSE)`:
  `#define MAXMEMSIZE 1048576 /* max memory size in 16-bit words */`),
  and the running `eclipse` binary's default config already matches it
  (`show cpu` → `1MW, STD`).
  - **This directly contradicts the "up to two megawords" figure** the
    request that started this investigation used. The math is exact
    (10-bit physical page field × 1024-word pages), not a rounding
    thing, and it's backed by both the address-translation formula and
    an explicit `#define`. If the real S/140's engineering documentation
    says otherwise, that's a discrepancy between this SIMH emulation and
    real hardware worth resolving before trusting any larger number —
    not something to paper over. Everything below is scoped to what
    this emulation actually does, since (per this project's own
    standing rule) nothing here is going near real hardware without
    verifying against it directly.
- **Map contexts**: `Map[8][32]` — 8 page-table contexts, each 32
  entries (one per logical page). `LoadMap()`'s switch on `(MapStat>>7)
  & 07` names them: 0/1/2/3 = user maps A/C/B/D (`Map[1]`/`Map[6]`/
  `Map[2]`/`Map[7]`), 4/5/6/7 = data-channel maps A/C/B/D (`Map[0]`/
  `Map[4]`/`Map[3]`/`Map[5]`) — used for DMA-capable I/O devices'
  own address translation (`MapAddr()`), not the CPU's own fetch/LDA/STA
  path; out of scope for Phase 1.
- **`LMP` (Load Map)**: fixed-encoding instruction, `IR == 0113410`
  (octal) in `eclipse_cpu.c`. Reads `AC1` words sequentially from memory
  starting at address `AC2` (each fetched via the *current* mapping —
  supervisor mode in every case here, so this is a plain direct read),
  adds `AC0` (a relocation constant — left 0 throughout Phase 1) to
  each, and loads the result into the map context selected by
  `MapStat`'s bits 7-9. Each loaded word's bits 10-14 select *which* of
  the 32 logical-page slots it becomes (`m = (w>>10)&037` inside
  `LoadMap`); bit 15 is a write-protect/valid flag; bits 0-9 are the
  physical page number (`MAPMASK = 0101777` = bit15 | bits[0-9] — the
  slot-selector bits are consumed, not stored). `AC0`/`AC1`/`AC2` are
  left at `0`/remaining-count/next-address on completion — matches a
  block-load instruction, not a single-shot one.
- **`DEV_MAP` (device `003` octal)**: `DOA` loads `MapStat` (also sets
  `Enable` — which user map, A or B, is "the" map for the mechanism
  below — from `MapStat`'s bit 2). `DIA` reads it back. `DOB` sets
  `Map31`, a fixed page used whenever a mapped program's own address
  reaches the top 1024-word block (`07600`-`077777`) or whenever
  supervisor mode itself addresses that block — the classic
  DG-OS "always-mapped shared top page" convention; unused, in scope for
  Phase 1's single-instruction test, not exercised. `DIC`/`DOC` are a
  "Page Check" side channel to read/write one specific map entry
  directly by index, independent of `LMP`'s bulk-load form — not used
  here either.
- **`NIOP MAP` + the very next memory reference = "single-cycle"
  mapping.** This is the actual mechanism Phase 1 uses, and it needs no
  user-mode context switch at all. Pulsing `DEV_MAP` with `P` while
  `Usermap == 0` (supervisor) sets `SingleCycle = Enable` (declared
  `/* Map one LDA/STA */` right at the top of the file — not a Phase-1
  guess, that's the simulator author's own comment). The *next*
  memory-referencing instruction (confirmed for `ELDA`/`ESTA` by reading
  their handlers directly: `if (SingleCycle) Usermap = SingleCycle;
  ...access...; if (SingleCycle) { Usermap = SingleCycle = 0; ...}`)
  executes under the armed map for that one access only, then reverts
  automatically. Supervisor code never actually leaves supervisor mode.

### A pleasant surprise: `dgasm` needed zero changes

Before touching anything I assumed adding MMPU support meant extending
`dgasm`. It doesn't: `LMP` is already a correctly-encoded opcode
(`opcode.c:204`: `{"LMP", 1, 0b1001011100001000, ENCODING_CONSTANT,
CPU_ECLIPSE_S140}` — `0b1001011100001000` = 38664 decimal = octal
`0113410`, exactly matching `eclipse_cpu.c`'s dispatch check). `DEV_MAP`
needs no special-casing either — it's just another device number, and
`dgasm` already supports arbitrary `dev NAME = code` declarations
generically (the same mechanism `boot_stage2.s.in` already uses for
`MTA`). So Phase 1 required **no `dgasm-src` changes at all** — the
regression baseline below is unchanged by construction, not just by
re-running it.

## Empirical verification

`examples/mmpu_probe.s` (new, committed): entirely supervisor-mode,
loads one page-table entry mapping logical page 0 (in user map A) to
physical page `050` octal (40 decimal — physical words 40960-41983,
comfortably past the 32768-word logical ceiling), arms single-cycle
mapping with `NIOP MAP`, and does exactly one `ESTA` of a marker value
(42) to a logical address inside that page. Assembled and run against
the real `eclipse_cpu.c`-based `eclipse` SIMH binary (no LLVM/backend
involvement):

```
$ dgasm -t eclipse_s140 -f simh -o mmpu_probe.simh mmpu_probe.s
$ { cat mmpu_probe.simh; echo 'dep PC 50'; echo 'step 20'; \
    echo 'e PC'; echo 'e 104'; echo 'e 120104'; echo 'quit'; } | eclipse

HALT instruction, PC: 00063 (JMP 0)
PC:	00063
104:	000000
120104:	000052
```

- `104` (octal) is `marker`'s own logical/plain-physical address —
  **unchanged (0)**: the single-cycle redirect touched nothing there.
- `120104` (octal) = physical page `050` (40 decimal) `<< 10` |
  `marker`'s page-0 offset (`0104` octal) — **`000052` octal = 42
  decimal**, exactly the value the one redirected `ESTA` wrote. This is
  a physical location no ordinary logical address (0-32767) can name —
  it is only reachable because the map entry pointed there.

This is the core claim, verified directly rather than asserted: **a
single Eclipse S/140 program can, via the MMPU, touch physical memory
that its own instruction encoding could never address directly.**

## Regression check

No existing file was modified — only `examples/mmpu_probe.s` (new) and
this document were added, so the existing baselines are unaffected by
construction. Re-run anyway for the record, since the historical
`regress_hwstack.sh`/`run_full_suite.sh`/`run_one_test.sh` scripts this
project's `DEBUGGING_NOTES.md` constantly refers to **no longer exist
anywhere in this checkout or in any repo's git history** (checked with
`git log --all` across every repo under `~/dev`) — they were apparently
never committed. That's a real reproducibility gap, worth fixing
separately from this investigation; the baseline used here is
`dgasm-src`'s own CTest suite plus the package's documented
`sizeof_check.c` smoke test, both unchanged before and after:

- `dgasm-src` CTest: **314/315 passing**, before and after (the one
  non-pass, `memcheck_hello`, is `Not Run` — `valgrind` isn't installed
  in this environment, unrelated to any code change).
- `examples/sizeof_check.c` through the full `eclipse-cc` → `eclipse`
  pipeline: byte-identical output before and after (`1 2 2 4 2 4 10
  14`, matching `README.md`'s documented expected result).

## What Phase 2 would actually need

Phase 1 proves the primitive works. It is not usable from C, and
nowhere close to "operating systems and other larger projects":

- **No LLVM/backend integration.** `ELDA`/`ESTA`/`EJSR`/`ELEF` codegen
  is untouched — a compiled C program still cannot address anything
  past 32767 no matter what the MMPU could reach for it. Using the
  MMPU today means hand-written assembly exactly like
  `mmpu_probe.s`, called from C only the way `eclipse-cc`'s multi-file
  support already allows (a separately-assembled `.s` file linked in) —
  there is no inline-asm story and no compiler-generated map management.
- **No C-callable API.** Nothing wraps `LMP`/`DOA MAP`/`NIOP MAP` as
  callable primitives (e.g. `mmpu_map_page(slot, physpage)`) — Phase 1
  is a single hand-sequenced probe, not a reusable library.
- **No page-fault handling.** `Fault`/`Check`/`DIC`/`DOC` (the "Page
  Check" read-back mechanism) exist in the emulation and are completely
  unexercised here. A real OS needs to catch and resolve faults, not
  just probe a pre-loaded, always-valid map entry the way this test
  does.
- **No user-mode context switching.** Everything here stays in
  supervisor mode via the single-cycle trick specifically *to avoid*
  needing to enter/exit user mode correctly. A real process model needs
  that (`Usermap = Enable` on some real context-switch path — not yet
  identified/verified here), plus `Map31`'s shared-top-page convention,
  plus interrupt-safe save/restore of map state (`MapIntMode` exists for
  exactly this and is untouched).
- **No multi-process story**, obviously — that's what all of the above
  would need to add up to.
- **Real-hardware caveat, same as everywhere else in this project**:
  verified only against this exact SIMH emulation of the S/140's MMPU,
  which the file's own header comment warns has "model-dependent
  quirks" across different real Eclipse models (the MODEL register
  exists specifically because of this). Nothing here has been checked
  against a real S/140's Programmer's Reference for the MMPU
  specifically, unlike the extended-addressing work in
  `DEBUGGING_NOTES.md` entries #22-23, which was. Given the 1MW-vs-2MW
  discrepancy already found, that check matters more here than usual.
