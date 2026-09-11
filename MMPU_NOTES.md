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
  1024-word pages = 1,048,576 words (1 megaword) as SIMH models it
  (`nova_defs.h:77`, `#define MAXMEMSIZE 1048576`, and `show cpu` →
  `1MW, STD`).
  - **Correction to this section's own history**: an earlier version of
    this note claimed the real ceiling was half that (524,288 words,
    9-bit page field), based on Chapter 2's prose and a page-table bit
    diagram found via a lossy OCR/text-search pass. That was wrong —
    the bit diagram belonged to the **Burst Multiplexor Channel (BMC)**,
    a separate optional DMA device (Chapter 3, device code `5` octal),
    not the core MMPU (device `MAP`, device code `3` octal) — same
    instruction *mnemonics* (`DIC`/`DOA`/`DOB`/`DOC`), completely
    different hardware, easy to conflate from text alone. Caught by
    going back to the actual scanned page images (not OCR text) for the
    core MMPU's own `LMP` instruction — see below.
  - **Verified against the real S/140 Programmer's Reference** (Data
    General pub. 014-000642-02, Rev. 02, April 1981 —
    bitsavers.org/pdf/dg/eclipse/014-000642-02_S140_PgmrRef_Apr81.pdf),
    by viewing the actual page images, not text extraction: the `LMP`
    (Load Map) instruction's own loaded-word format, page "5-84",
    states plainly:
    ```
    Bits   Name            Contents or Function
    0      WRITE PROTECT   Must be 0 for data channel maps; 1 for user maps.
    1-5    LOGICAL         Logical page number.
    6-15   PHYSICAL        Physical page number.
    ```
    Bits 6-15 is **10 bits** — physical page numbers 0-1023, exactly
    matching `PAGEMASK`. 1024 pages × 1024-word pages = **1,048,576
    words (1 megaword) — the core MMPU's real addressing capability
    matches this SIMH emulation exactly.** This is the mechanically
    authoritative source (the literal bit format the hardware decodes
    when `LMP` loads a map register), weighted above Chapter 2's prose
    description below, which has an unresolved internal inconsistency.
  - **A separate, also-real number**: Chapter 1's product overview
    states the S/140 as actually sold topped out at "1 Mbyte (up to
    eight boards)" of *installed* RAM — 524,288 words. That's a real
    fact, but it answers a different question (how much physical memory
    any real, historically-shipped S/140 unit could actually have
    populated) than the MMPU's addressing capability above (how far its
    page table can point, whether or not memory is actually there).
    Both are true; they're not in tension once kept separate. Chapter
    2's own prose ("physical address space is 1,048,576 **bytes**
    (1M)... addressed by a 19-bit address") is internally inconsistent
    under any unit reading — 2^19 = 524,288, not 1,048,576, as bytes or
    as words — likely a genuine error/looseness in the 1981 document;
    not resolved further here, and not trusted over the `LMP` bit table.
  - **Net effect on the "two megawords" premise that started this
    investigation**: the real number is 1 megaword (1,048,576 words),
    half the original premise, matching SIMH exactly — not a further
    correction down to 512K as this note previously (and wrongly)
    concluded. `examples/mmpu_probe.s`'s physical page 40 is safely
    inside every version of this range regardless, so its empirical
    result is unaffected by any of this back-and-forth.
  - **One correction from the earlier pass does still stand**: the real
    S/140 has only **two** user maps (A/B) — Chapter 2's "Types of
    Maps" section, confirmed on the same page-image pass as the `LMP`
    table above: *"The MMPU can hold two user maps, but only one can be
    enabled at any one time."* The `Map[8][32]` array's slots for user
    maps C/D exist in `eclipse_cpu.c` for *other* Eclipse models (the
    file's own header comment already flagged this). Four data-channel
    maps (A-D) *is* correct for the S/140 per the same document.
- **Map contexts**: `Map[8][32]` — 8 page-table contexts, each 32
  entries (one per logical page). `LoadMap()`'s switch on `(MapStat>>7)
  & 07` names them: 0/1/2/3 = user maps A/C/B/D (`Map[1]`/`Map[6]`/
  `Map[2]`/`Map[7]`), 4/5/6/7 = data-channel maps A/C/B/D (`Map[0]`/
  `Map[4]`/`Map[3]`/`Map[5]`) — used for DMA-capable I/O devices'
  own address translation (`MapAddr()`), not the CPU's own fetch/LDA/STA
  path; out of scope for Phase 1. **On the real S/140, only user maps A
  and B exist** (see the correction above) — `Map[6]`/`Map[7]` (C/D)
  are real in this emulation but model other Eclipse variants, not this
  one; don't use them for anything meant to reflect real S/140
  behavior. The four data-channel maps (A-D) are real for the S/140.
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
  past 32767 no matter what the MMPU could reach for it.
- **C-callable API: done — see "Phase 2: a C-callable API" below.**
  (This bullet previously claimed `eclipse-cc`'s multi-file support
  already allows a separately-assembled `.s` file to be linked in
  alongside compiled C. That turned out to be wrong — checked directly
  against `eclipse-cc`'s own source before relying on it: every
  element of its `sources` array unconditionally goes through `clang
  -cc1 -emit-llvm`, which cannot process raw target assembly text. The
  *only* existing mechanism for combining hand-written assembly with
  compiled C is a single hardcoded `cat "$HWFLOAT_SRC" >> "$asm"` for
  exactly one file, `rt/eclipse_hwfloat.s` — not a general feature.
  Phase 2 uses inline `asm volatile(...)` instead, the same mechanism
  `examples/fps.h` already uses for the FPS100 driver, which needs no
  `eclipse-cc` changes at all.)
- **Page-fault handling: done for detection — see "Phase 2: page-fault
  handling" below.** A real validity fault, triggered from genuine user
  mode, correctly reaches a handler at the documented location with a
  correct, directly-verified 5-word return block. Recovery/resume is
  explicitly not attempted — the manual itself says the returned PC
  isn't reliably correct for this fault class — and write-protection
  faults specifically, `DIC`'s "Page Check" *read-back by the handler*
  (this increment only used `DIA`), and `Check`/`MapIntMode` remain
  unexercised.
- **User-mode context switching: done — see "Phase 2: real user-mode
  context switching" below.** Real, sustained Usermap != 0 execution
  (not the single-cycle trick), entered via an indirect-reference
  trigger and exited via `NIOP`, verified across multiple instructions
  with the simulator's own instruction trace. Still open: protection
  faults (this test's map entries are always valid, never exercises
  `Fault`), `MapIntMode`'s interrupt-safe save/restore of map state
  across a real interrupt, and any second-user (map B) or
  process-switching story — see that section's own closing bullet.
- **No multi-process story**, obviously — that's what all of the above
  would need to add up to.
- **Real-hardware caveat, fully updated**: every core-MMPU (`MAP`
  device) instruction's dictionary entry has now been checked directly
  against the manual's own page images (not OCR) — `LMP`, `DIA` (Read
  Map Status), `DIC` (Page Check), `DOA` (Load Map Status), `DOB` (Map
  Page 31), `DOC` (Initiate Page Check), `NIOP` (Map Single Cycle).
  Results:
  - **1-megaword memory size, triply confirmed**: `LMP`'s loaded-word
    format, `DOB`'s "Map Page 31" AC format, and `DOC`'s "Initiate Page
    Check" AC format all independently use the same 10-bit PHYSICAL
    field pattern. All three agree with `PAGEMASK`/`MAXMEMSIZE`.
  - **2-user-map limit, now confirmed by the hardware's own encoding
    table, not just prose**: `DOA`'s Map Select field and `DOC`'s Map
    field (both 3-bit, bits 6-8) list all 8 possible codes identically
    — `000`=User A, `010`=User B, `100`-`111`=the four data-channel
    maps, and **`001`/`011` explicitly marked "Reserved."** That's the
    real hardware's own instruction format declaring the "extra"
    user-map codes unused, stronger evidence than the earlier prose
    citation alone.
  - **`NIOP`'s behavior matches `eclipse_cpu.c`'s `SingleCycle`
    mechanism** as this document already described it from source: "the
    instruction maps one memory reference using the last user map" —
    no correction needed to that section.
  - **Still unverified**: everything about page faults, user-mode
    context switching, and `MapIntMode` — none of Phase 2's harder
    problems have been checked against the manual yet.
  Given that the *previous* correction pass in this file itself
  contained an error (see the memory-size bullet's history above),
  the standing rule for this document is: trust a claim here only as
  far as its citation — a page/table reference means it was checked
  against that image directly; anything without one hasn't been.

## Phase 2: a C-callable API

`examples/mmpu.h`/`mmpu.c`: `int mmpu_read_far(unsigned int physpage,
unsigned int offset)` and `void mmpu_write_far(unsigned int physpage,
unsigned int offset, unsigned int value)` — a real, reusable,
parameterized version of `mmpu_probe.s`'s single hand-sequenced probe,
callable from ordinary C. Still entirely supervisor-mode, still no
page-fault handling, still no user-mode context switching — same
explicit scope as Phase 1, just made reusable.

**Design, and why it looks the way it does:**

- Every value crosses the C/asm boundary through a named global
  (`_mmpu_pte`, `_mmpu_target`, `_mmpu_value`), never through an
  operand-substituted register. `examples/fps.h`'s `fpu_out`/`fpu_in`
  use `"r"`-constrained operands successfully, but that works because
  `DOA`/`DOB`'s accumulator is a genuine operand slot the register
  allocator can fill with anything. `LMP` is different: `AC0`
  (relocation), `AC1` (count), and `AC2` (source address) are fixed by
  *architectural convention*, not operand-encoded — bare `LMP` takes
  zero operands at all. Mixing `"r"`-constrained operands with
  hardcoded `AC0`-`AC2` clobbers in one asm block risks a collision
  this backend's inline-asm has no confirmed way to prevent (no
  clobber-list or fixed-register constraint support verified to
  exist). Routing everything through memory sidesteps the question
  entirely — confirmed empirically that a C global's name is not
  mangled, so `ELDA`/`ESTA ...,_mmpu_pte,0` (absolute addressing)
  reaches it directly. Plain `LDA`/`STA` (2-operand, page-zero-only)
  do *not* work for this — confirmed empirically the hard way (a
  "Address out of range, should be 0-255" `dgasm` error) once these
  globals landed outside page zero; `ELDA`/`ESTA` with explicit
  absolute addressing is required.
- `asm("dev MAP = 03");` at **file scope**, not inside either
  function's own asm block. A `dev` declaration inside a function body
  only survives if that specific function survives dead-code
  elimination; two independent copies (one per function, if both
  survive) is a hard `dgasm` error ("Multiple definitions for symbol
  MAP") — confirmed empirically. Module-level `asm(...)` is emitted
  unconditionally regardless of which functions survive — also
  confirmed empirically — so there is exactly one declaration, correct
  regardless of which of `mmpu_read_far`/`mmpu_write_far` a caller
  actually uses.
- `ADI`'s immediate operand is the literal amount to add (1-4), not a
  0-based code — `ADI 1,1` adds 1 to AC1; `ADI 1,0` is a `dgasm` range
  error ("Immediate out of range. Must be 1-4, got 0"). Confirmed by
  reading `dgasm`'s own `encode_immediate_instruction`/`get_short_imm`
  after hitting that error the first time.
- **The real bug this took longest to find**: an early version
  clobbered `AC2` (via `ELEF`/`ELDA`, to build the `LMP` source address
  and the redirect target) without restoring it. `AC2` is not just
  "some register" here — it's this backend's live frame pointer for
  the *entire* function body (every `LDA/STA n,2` frame-relative
  access uses it), confirmed by single-stepping the broken version in
  `eclipse` and watching `AC0` change from the correct return value
  (`010341` octal / 4321, present right up through the function's own
  `RTN`) to a stale, unrelated value immediately after — traced to
  `RTN`'s own handler in `eclipse_cpu.c` (confirmed by reading it
  directly): it unconditionally restores `AC[0]`-`AC[3]` from the
  stack block `SAVE` pushed at entry, which is architecturally correct
  (see the manual's own Stack Instructions table), but means the
  *compiler-generated* code between the asm block and `RTN` — which
  stores the real return value to a frame-relative stack slot — was
  using the clobbered `AC2` as its base address, silently storing
  the real result nowhere the caller ever reads it. Fixed by saving
  `AC2`/`AC3` (via `ESTA`, to `_mmpu_save2`/`_mmpu_save3`) as literally
  the first two instructions in each asm block, and restoring them as
  the last two, before falling back into compiler-generated code that
  assumes `AC2` is still the frame pointer. `AC3`'s save/restore is
  defensive, not confirmed-necessary the same way `AC2`'s was.
- `_mmpu_save2`/`_mmpu_save3` need a dummy plain-C write
  (`_mmpu_save2 = 0;` right before the asm block) or `dgasm` reports
  "Undefined symbol" — confirmed empirically. With *no* visible C-level
  reference at all (only asm-text references, invisible to the
  compiler's own liveness analysis), `globaldce` removes the global's
  storage entirely. `_mmpu_pte`/`_mmpu_target`/`_mmpu_value` never hit
  this because each is genuinely written from plain C before its own
  asm block runs.

**Empirical verification**, through the *real* `eclipse-cc` pipeline
(clang -cc1 → llvm-link → opt → llc → reorder_asm.py → dgasm), not
hand-assembled like `mmpu_probe.s`:

- `examples/mmpu_far_test.c`: `mmpu_write_far(0100, 0200, 4321)` then
  `mmpu_read_far(0100, 0200)` → prints `4321`. Direct memory-dump
  confirmation after running: `e 200200` (physical page `0100` octal
  `<< 10 | 0200` octal) shows `010341` octal (4321); `e 200` (the
  same offset in plain, unmapped logical space) shows a small,
  unrelated value, confirming the write only ever touched the far
  physical address.
- `examples/mmpu_far_multi_test.c`: two *different* physical
  pages/offsets (`0300,0700` → 9999; `0044,0033` → 111), plus a repeat
  read of the first address after the second call — all three correct
  (`r1=9999`, `r2=111`, `r1b=9999`), confirming no state leaks between
  calls to different physical locations.
- Isolated read path (not committed — a throwaway test during
  debugging, described here for the record): pre-seeded the target
  physical address directly via SIMH `dep` (bypassing
  `mmpu_write_far` entirely) and confirmed `mmpu_read_far` alone
  retrieves a pre-existing value — ruled out any write/read
  interaction as the source of the `AC2` bug before finding it.

**Regression check**: `dgasm-src` CTest, re-run after these changes:
**315 tests, 314 passing, 1 `Not Run`** (`memcheck_hello`, still
`valgrind`-not-installed, identical to Phase 1's baseline — this
change touched zero `dgasm-src` files, so this is expected, not just
hoped for). `examples/sizeof_check.c` through the full pipeline:
byte-identical (`1 2 2 4 2 4 10 14`) before and after.

**What Phase 2 still doesn't have**: LLVM codegen integration, page
fault handling, and user-mode context switching — see the list above,
now with exactly one item checked off.

## Phase 2: real user-mode context switching

`examples/mmpu_usermode_probe.s`: proves genuine, sustained Eclipse
S/140 user-mode execution — `Usermap` actually nonzero across multiple
instructions — not Phase 1/2's single-cycle trick, which never leaves
supervisor mode at all.

**The real danger, confirmed in source before designing around it**:
once `Usermap != 0`, `eclipse_cpu.c`'s main loop fetches *every*
instruction through the map (`IR = GetMap(PC)`, read directly, not
assumed) — unlike the single-cycle mechanism, which only ever redirects
one data access. Getting this wrong risks the CPU fetching garbage for
its own next instruction. This program avoids the risk rather than
handling it: it stays entirely within logical/physical page 0 (`org
050`) and loads an identity map entry there (logical page 0 -> physical
page 0), so code/data in that page reads identically regardless of
`Usermap`'s value — the transition is invisible to normal execution. A
second, deliberately non-identity entry (logical page 2 -> physical
page 0150 octal) is what actually demonstrates translation is real.

**Trigger, confirmed in source, not just the manual**: the manual's
`DOA` ("Load Map Status") bit 15/User Enable says the switch happens
"on the first memory reference after the next indirect reference or
return type instruction." Reading `eclipse_cpu.c`'s `effective()`
directly confirms it precisely: its indirect-chain loop does `MA =
GetMap(...); if (MapStat & 1) { Usermap = Enable; Inhibit = 0; }` on
every indirect fetch — so a plain `LDA 0,@iptr` is a real, minimal,
controllable trigger, avoiding the stack setup `POPJ`/`RTN`/etc (the
manual's other documented triggers) would need.

**A subtlety that would have silently broken a naive design**: the
manual's "Unmapped Mode" section describes logical page 31 as always
specially mapped via a separate `Map31` register — but `GetMap`'s own
`case 0` (Usermap==0) vs `case 1`/`case 2` (real user maps) confirm
that only applies while `Usermap==0`. Once genuinely in user mode, page
31 is governed by the ordinary per-map `Map[ctx][31]` entry like every
other page, not `Map31`. This program never references page 31 at all,
sidestepping the question rather than relying on an unloaded/stale
`Map[1][31]` entry behaving usefully.

**Return path, confirmed in source**: the manual's `NIOP` entry ("Map
Single Cycle / Disable User Mode") documents dual behavior depending on
current mode — from supervisor, it arms single-cycle mapping (Phase
1/2's mechanism); "from user mode — if LEF mode and I/O protection are
disabled, this instruction turns off the MMPU." Confirmed directly in
`eclipse_cpu.c`'s `DEV_MAP` pulse handler: `if (Usermap) { MapStat &=
0177776; Usermap = 0; Inhibit = 0; } else { SingleCycle = Enable; ...
}` — the same instruction, opposite effect, depending on whether
`Usermap` is already active.

**A real bug found and fixed, the same way Phase 2's `AC2` bug was —
by not trusting the first result**: `var farlogaddr = 04200`, intended
as a compile-time constant for `ESTA`'s absolute-address operand,
turned out to allocate a real storage *word* instead — confirmed by
reading `dgasm-src/assembler.c`'s `VARIABLE_NUMBER` case directly:
`buffer[current_addr] = eval(...)`, i.e. `var X = N` is exactly
equivalent to `X: dw N`, not a symbolic alias with no storage cost.
`ESTA` then encoded the *address of that word* (0110 octal) rather
than `04200` itself. Caught by enabling `eclipse_cpu.c`'s own built-in
per-instruction trace (`d debug 100003`, then reading `trace.log`) and
noticing `ESTA 1,110` where `ESTA 1,4200` was expected. Fixed by using
the literal `04200` directly in the instruction instead of a named
`var`.

**A real SIMH console limitation found and worked around, not silently
avoided**: this `eclipse` binary's `DEVICE` struct declares
`awidth=17` (`eclipse_cpu.c`'s `cpu_dev`), so the interactive console's
`e`/`d` commands cannot address physical memory at or above 128K words
(`0400000` octal) — confirmed empirically by bisection (`e 377777`
works, `e 400000` doesn't) and unaffected by `set cpu 1024k`. This has
no effect on a *running program's* own `GetMap`/`PutMap` (which touch
`M[]` directly in C, not through this console path) — only on
interactively examining physical memory above that threshold. This is
likely why Phase 2's own far-page tests (`mmpu_far_multi_test.c`,
physical pages up to 0700 octal — which, computed out, land past this
same 128K boundary) verified via the C program's own printed output
rather than console `e`: they would have hit this same wall. This
test's far physical page (0150 octal) was chosen small enough to stay
under the ceiling instead, matching `mmpu_probe.s`'s own precedent,
rather than changing the underlying `awidth` declaration (a SIMH
source change, out of scope here).

**Empirical verification**, with the simulator's built-in instruction
trace enabled (`d debug 100003`) to show the `Usermap` transition
directly, not just infer it from before/after memory state:

```
$ dgasm -t eclipse_s140 -f simh -o mmpu_usermode_probe.simh mmpu_usermode_probe.s
$ { cat mmpu_usermode_probe.simh; echo 'dep PC 50'; echo 'step 40'; \
    echo 'e PC'; echo 'e 4200'; echo 'e 320200'; echo 'quit'; } | eclipse

HALT instruction, PC: 00065 (JMP 0)
PC:	00065
4200:	000000
320200:	013056
```

- `4200` (octal) = logical page 2, offset 0200 — examined after
  returning to supervisor mode, where logical=physical directly:
  **0, untouched** — the write never touched physical page 2.
- `320200` (octal) = physical page 0150 octal `<< 10` | offset 0200 —
  **013056 octal = 5678 decimal**, exactly the value `ESTA` wrote while
  genuinely in user mode.

The instruction trace shows the mechanism directly, not just the
before/after result:
```
  000057 acs: 000000 000000 000105 000000 0 LDA 0,@105
 A000060 acs: 001747 000000 000105 000000 0 LDA 1,107
 A000061 acs: 001747 013056 000105 000000 0 ESTA 1,4200
 A000063 acs: 001747 013056 000105 000000 0 NIOP 0,MAP
63 NIO 0 (No I/O, clear faults)
63 xxxP (Single Cycle)
  000064 acs: 001747 013056 000105 000000 0 HALT
```
The leading `A` — this project's own trace format for "Usermap==1",
confirmed in `eclipse_cpu.c`'s trace code (`if (Usermap==1)
strcpy(debmap,"A")`) — appears starting exactly at PC `000060`, the
instruction immediately after the indirect-reference trigger, and
persists across `LDA`, `ESTA`, and `NIOP` (three consecutive
instructions, not a single blip), then disappears for the final
`HALT`: direct proof that real, sustained user-mode translation was
active for more than one instruction and was cleanly deactivated by
`NIOP` — not just an inference from memory contents.

**Regression check**: `dgasm-src` CTest, re-run after this change:
**315 tests, 314 passing, 1 Not Run** (`memcheck_hello`, unchanged,
`valgrind` still absent — this change touched zero `dgasm-src` files).
`examples/sizeof_check.c` through the full `eclipse-cc` pipeline:
byte-identical (`1 2 2 4 2 4 10 14`).

**What this still doesn't cover**: protection faults (this test's map
entries are always valid, deliberately never triggers `Fault`),
`MapIntMode`'s interrupt-safe save/restore of map state across a real
interrupt, any second-user (map B) or process-switching story, and
(still, as always) LLVM codegen integration. Given how many real,
non-obvious findings turned up in *this* increment alone (the `var`
storage-allocation bug, the console `awidth` limit, the page-31
subtlety) despite starting from a fully-verified Phase 1/2 base, the
standing rule from the memory-size correction saga applies with extra
force here: don't assume the next increment (page faults especially)
will be as clean as this one turned out to be once actually attempted.

## Phase 2: page-fault handling

`examples/mmpu_fault_probe.s`: a user-mode program deliberately
triggers a real MMPU validity protection fault (building directly on
`mmpu_usermode_probe.s`'s entry mechanism) and a real supervisor-mode
handler, installed at the manual's documented location, correctly
receives control with correct, directly-verified state.

**Why validity, not write protection**: both are always reachable from
user mode, but validity needs no extra enable bit — confirmed directly
in `eclipse_cpu.c`'s `GetMap` (case 1, User A): `if (Map[1][page] ==
INVALID && !SingleCycle) Fault = 0100000;`, no `MapStat`/WP-enable
check at all, matching the manual's Ch. 2 p.2-30 "Validity protection is
always enabled." Write protection additionally needs `MapStat`'s WP
bit set — one more moving part this increment didn't need.

**The "declare invalid" encoding, checked by arithmetic and then by
the emulator's own trace, not assumed**: the manual's NOTE under `LMP`'s
word format says "Declare a logical page invalid by setting the write
protect bit to 1 and all of bits 6-15 to 1." For logical page 2:
bit0(WP)=1, bits1-5(LOGICAL)=2, bits6-15(PHYSICAL)=all-1s → `0105777`
octal as the word fed to `LMP`. After `LoadMap`'s masking
(`Map[ctx][m] = w & MAPMASK`), this lands as exactly `INVALID`
(`0101777`, `eclipse_cpu.c`'s own sentinel) — confirmed both by hand
arithmetic (`0105777 & 0101777 = 0101777`) and by the simulator's own
built-in `LMP` trace line, which independently decodes the loaded word
and printed exactly `MAP L=2 W=1 P=1777` — matching the intended
encoding from a second, independent source.

**Fault dispatch mechanism, read directly from `eclipse_cpu.c`'s main
loop** (the `if (Fault) {...}` block, checked once per *completed*
instruction, not mid-instruction):
```
Usermap = 0;                    // current user map disabled
MapStat &= ~01;                 // MMPU itself disabled
if (Fault & 0100000) MapStat &= ~0170;   // only for Fault==0100000 (Validity)
MapStat |= Fault & 077777;      // fault code merged into MapStat
<push AC0, AC1, AC2, AC3, PC as a 5-word return block, via PutMap>
PC = indirect(M[003]);          // JMP to loc 3 -- RAW M[3], not GetMap(3)
```
That last line is worth being precise about: it reads location 3 as
flat physical memory, not through the (already-disabled) map — direct
confirmation that Ch. 2's "unmapped logical address space" for
locations 0-3 (Table 2.14) means what it says, not just conceptually.

**Empirical verification**, with the simulator's own instruction trace
(`d debug 100003`) plus direct memory examination of the pushed return
block:
```
$ dgasm -t eclipse_s140 -f simh -o mmpu_fault_probe.simh mmpu_fault_probe.s
$ { cat mmpu_fault_probe.simh; echo 'dep PC 50'; echo 'step 40'; \
    echo 'e 40'; echo 'e 61'; echo 'e 62'; echo 'e 63'; echo 'e 64'; echo 'e 65'; echo 'e 112'; \
    echo 'quit'; } | eclipse

40:	000065      -- stack pointer, advanced by exactly 5 (060 -> 065)
61:	001747      -- AC0 at fault time
62:	000000      -- AC1 at fault time
63:	000110      -- AC2 at fault time
64:	000000      -- AC3 at fault time
65:	000070      -- saved PC: the faulting ELDA's own address (000066) + 2
                     (it's a 2-word instruction) -- the instruction
                     *after* the fault, matching the manual's general
                     description
112:	000000     -- mapstat_after: DIA's read-back of MapStat post-fault
```
Cross-checked against the trace directly (not just the final memory
dump): AC0/AC1/AC2/AC3 at addresses 61-64 match the trace's own printed
register state in the instruction immediately before the fault
(`001747 000000 000110 000000`) exactly. More tellingly, the trace
shows the faulting `ELDA` prefixed with `A` (this project's own
"Usermap==1" marker, same convention `mmpu_usermode_probe.s`
established) and the very next line — `DIA 0,MAP` — has no `A` prefix
at all, and its own address (`000071`) is `pf_handler`'s real address,
*not* `000070` where the never-reached `HALT` sits. That's direct proof
the fault fired, the map was disabled, and control reached the
installed handler — not an inference from before/after state alone.

**A real finding, confirmed empirically rather than left as a
derivation**: `DIA` (Read Map Status) read back `MapStat = 0` after
this validity fault — *not* with the manual's described "last fault
was write-or-validity" (`WP`) status bit set. Working through why,
from the dispatch code above: `MapStat` was `1` (from `DOA`'s earlier
User-Enable write) going in; `&= ~01` clears it to `0`; `Fault &
077777` for `Fault == 0100000` (pure validity, bit 15 only) is `0`, so
the OR-in sets nothing further. Net: `0`. This means — in *this*
emulation, for *this* specific fault path — the "last fault type"
status bit the manual describes for `DIA` does not actually get set on
a validity fault; only `Fault`'s low 15 bits (which is where the
*write-protect* fault code, `010000`, actually lives) would show up
via this mechanism. Whether that's a genuine emulator inaccuracy versus
real S/140 hardware, or whether the manual's `DIA` bit-3 description
applies to a different code path than the one exercised here, is not
resolved — flagged as a real, citable discrepancy rather than smoothed
over, consistent with this document's standing rule.

**What this deliberately does not attempt: recovery/resume.** The
manual itself (Ch. 2, p.2-30) says plainly: *"A protection fault can
occur at any point during the execution of an instruction. Therefore,
the return address in the fifth word of the return block is not always
correct. For I/O protection faults, however, the fifth word will
always be the logical address of the instruction following the
instruction that caused the fault."* Validity/write faults are
explicitly *not* the I/O case that gets a guaranteed-correct return
PC. A generic "just `RTN` back and continue" demo would misrepresent
something DG's own manual flags as unreliable for exactly this fault
class — so this increment stops at "fault occurs, handler receives
correct, verified state," per the standing scope this document already
uses for open-ended items.

**A real bug in this test itself, caught and fixed**: `dw 177777`
(intended as octal, for the stack limit) has no leading zero, so
`dgasm` parsed it as *decimal* 177777, wrapping to `133161` octal
(46705 mod 65536) — the exact same "octal needs a leading zero" gotcha
`mktape.py`'s own header comment already documents elsewhere in this
project, which this test forgot to apply. Caught via the instruction
trace showing `133161` loaded instead of the intended value; fixed by
writing `0177777`. Confirmed via re-run that this had zero effect on
the fault-handling result itself (the stack limit is only consulted
for overflow *protection*, never exercised by this test's single
5-word push).

**Regression check**: `dgasm-src` CTest, re-run after this addition:
**315 tests, 314 passing, 1 Not Run** (`memcheck_hello`, unchanged,
`valgrind` still absent — this change touched zero `dgasm-src` files).
`examples/sizeof_check.c` through the full `eclipse-cc` pipeline:
byte-identical (`1 2 2 4 2 4 10 14`).

**What's still open**: genuine recovery/resume after a fault (explicitly
out of scope here, per the manual's own reliability caveat above),
`MapIntMode`'s interrupt-safe save/restore, write-protection faults
specifically (not attempted — validity was chosen as the simpler of
the two), any second-user/process-switching story, and (still, as
always) LLVM codegen integration.
