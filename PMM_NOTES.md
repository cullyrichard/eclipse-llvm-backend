# PMM (Physical Memory Manager) — a physical page frame allocator

Builds directly on `MMPU_NOTES.md`'s Phase 2 C-callable API
(`examples/mmpu.h`/`mmpu.c`: `mmpu_read_far`/`mmpu_write_far`), which
proved a compiled C program can touch real physical memory past the
32768-word logical ceiling but did nothing to track *which* physical
words are free — every prior MMPU test hand-picked its own physical
addresses. This is the first piece of this project that a real kernel
could actually call to get a physical frame on demand: `examples/pmm.h`/
`pmm.c` (the allocator) and `examples/pmm_test.c` (real, hand-verified
proof it works, including exhaustion behavior).

## 1. Page size and frame count, re-derived and cross-checked

`MMPU_NOTES.md`'s Phase 1 section already established these numbers
against the real S/140 Programmer's Reference (014-000642-02 Rev 02,
Apr 1981) via the `LMP` instruction's page-image bit table, and
triply-confirmed them against `DOB`/`DOC`'s AC formats too. This section
re-derives the same numbers independently, directly from SIMH's own
source (`~/dev/simh-src/NOVA/`), the primary source for how the
*emulation* actually behaves (as opposed to how the real 1981 hardware's
manual describes it) — both were checked, not just one quoted from the
other:

- `nova_defs.h:77`: `#define MAXMEMSIZE 1048576` — total physical words
  modeled, 1,048,576 (1 megaword), confirmed by direct grep, not
  transcription from `MMPU_NOTES.md`.
- `eclipse_cpu.c:424`: `#define PAGEMASK 01777` — 01777 octal = 1023
  decimal = 10 bits all set. This is the physical-page-number mask
  every `GetMap`/`PutMap` case applies (`(Map[ctx][page] & PAGEMASK) <<
  10`) — confirmed by grep across every case (`GetMap`'s cases 0/1/2/6/7
  and `PutMap`'s equivalents all use it identically).
- **Page size, computed two independent ways that must agree, and do**:
  - `1,048,576 words total / 1024 possible physical pages (PAGEMASK+1)
    = 1024 words/page.`
  - Independently, the `<< 10` in every `GetMap`/`PutMap` case *is* the
    page-size declaration by itself: shifting a page number left by 10
    bits to get its base physical address means each page is
    `2^10 = 1024` words, with no need to even know the total memory
    size to derive it. Both routes land on the same number — not a
    coincidence, since `PAGEMASK`'s 10 bits and the `<<10` shift width
    are the same design constant used twice, but worth checking they
    weren't accidentally out of sync in the source, which they aren't.
  - This matches `MMPU_NOTES.md`'s own citation of the manual's `LMP`
    bit table (bits 6-15, 10 bits, PHYSICAL field) exactly.
- **Frame count**: 1024 total physical page frames, numbered 0-1023.

**No new physical-memory-size claim is made here that `MMPU_NOTES.md`
didn't already establish against the real manual** — this section's
contribution is checking the same numbers against a second, independent
primary source (the emulator's own C source) and confirming they agree,
per this document's own standing rule (a page-image-image citation
outranks prose, but two independently-checked sources agreeing is
stronger evidence than either alone).

## 2. Reserved range: why frames 0-31 are never handed out

The allocator does not hand out every one of the 1024 frames — physical
pages 0-31 (physical words 0-32767, exactly the 32768-word logical
window) are permanently reserved. Why that boundary, checked directly
in `eclipse_cpu.c`'s `GetMap`, case 0 (`Usermap == 0`, i.e. ordinary
supervisor-mode execution — what every `eclipse-cc`-compiled program
runs under, since none of Phase 2's user-mode work is used by ordinary
compiled C programs):

```c
case 0:
    if (addr < 076000)
        return M[addr];
    paddr = ((Map31 & PAGEMASK) << 10) | (addr & 001777);
    ...
```

`076000` octal = 31744 decimal = page 31's own start (`31 * 1024`). So:

- **Logical pages 0-30** (words 0-31743) are a **direct, unconditional
  identity mapping** (`M[addr]`) whenever a program runs in ordinary
  supervisor mode — i.e. every physical word in this range is exactly
  where the *running program itself* (its code, static data, and stack
  — an Eclipse program's entire address space, since this backend has
  no separate heap/mmap mechanism) already lives, or could reach by
  ordinary addressing. Handing any of these frames to the allocator
  would risk it giving out a frame the loaded program is actively using
  for its own code or stack.
- **Logical page 31** (words 31744-32767) is a **different, special
  case** — routed through `Map31`, a separate single register (the
  manual's documented "always-mapped shared top page" convention), not
  a direct identity map. `Map31` defaults to 0 unless explicitly loaded
  (nothing in this project's examples ever loads it), so in practice an
  ordinary program's access to logical page 31 — if it ever made one —
  would silently alias back onto physical page 0's own words, *not*
  physical page 31. This means physical page 31 itself is very likely
  never actually touched by ordinary running programs. **Reserved
  anyway**, for two reasons: (a) location 0-3 physical words are used
  unconditionally by the hardware itself regardless of `Usermap` — e.g.
  `MMPU_NOTES.md`'s page-fault dispatch section confirms location 3 is
  read as **raw physical memory** (`M[3]`, not through any map) for the
  fault-handler vector, and classic Nova/Eclipse convention places
  device interrupt vectors in the low words of page 0 too — all safely
  inside pages 0-31 either way; (b) keeping the reserved region a clean,
  round 32-page/32768-word block (exactly the logical ceiling) is a
  simple, conservative, easy-to-verify-by-inspection design choice, and
  the cost is small — see the tradeoff note below.

**Net reservation**: frames 0-31 (32 frames, 32768 words) reserved;
frames 32-1023 (`PMM_NUM_FRAMES` = 992 frames, 1,015,808 words)
allocatable.

**Real constraint worth flagging for OS scoping**: even though the
*addressable* physical memory is a full megaword, the fraction any
compiled program's own footprint can occupy is capped at exactly
32768 words (32 frames) — the same iron 32K logical ceiling that
originally motivated the whole MMPU investigation (`MMPU_NOTES.md`'s own
opening paragraph). The MMPU/allocator combination gives a kernel
**994528 more words of storage to hand out to processes as data**, but
does *not* relax that ceiling for any single program's own instructions
or automatically-addressed stack/globals — a process still needs the
same single-cycle or user-mode MMPU tricks Phase 1/2 already proved
just to reach any frame this allocator hands out beyond its own 32K
window. This allocator is the bookkeeping half of "physical memory
management"; it does not by itself give a process a bigger *logical*
window — that's still bounded by the same 2-user-map (A/B) hardware
limit `MMPU_NOTES.md` already flagged. A larger OS design built on this
allocator needs to reckon with: any physical frame it hands to a
process is only reachable through one of exactly two live user maps at
a time (no per-process unlimited page tables the way a modern MMU would
give you) — multiplexing more than a couple of live "big" address
spaces means swapping user-map contents, not just picking a free frame.

## 3. Allocator design (`examples/pmm.h` / `examples/pmm.c`)

```c
#define PMM_FIRST_FRAME 32
#define PMM_LAST_FRAME  1023
#define PMM_NUM_FRAMES  (PMM_LAST_FRAME - PMM_FIRST_FRAME + 1)  /* 992 */
#define PMM_NONE (-1)

void pmm_init(void);
int  pmm_alloc_page(void);       /* returns a physpage number, or PMM_NONE */
void pmm_free_page(int pfn);
int  pmm_frames_free(void);      /* diagnostic/test-only */
```

A returned `pfn` is exactly the `physpage` argument
`mmpu_read_far`/`mmpu_write_far` (`examples/mmpu.h`) expect — no
translation between the two APIs, by design.

**Free-list representation: a plain byte array, not a packed bitmap —
a deliberate tradeoff, not an oversight.** `static unsigned char
freemap[PMM_NUM_FRAMES]` (992 bytes), `freemap[i] != 0` meaning frame
`i` is free; `pmm_alloc_page()` does a linear first-fit scan (lowest
free index wins), `pmm_free_page()` sets one byte back to 1. The
"obvious" denser design — one bit per frame, 62 sixteen-bit words
instead of 992 bytes — was considered and rejected: this backend has a
**confirmed real bug class** around 16-bit shift arithmetic landing at
or past the sign bit. `examples/mmpu_far_test.c`'s own comment
documents `physpage<<10 | offset` overflowing this target's signed
16-bit `int` and triggering the compiler's own `-Wshift-overflow`
warning at a similar magnitude (bit 15) to what a packed bitmap's own
per-word `1 << bit_index` (bit_index up to 15) would need. A
foundational, correctness-critical structure like a physical allocator
is exactly the wrong place to spend that specific, already-demonstrated
risk to save under a kiloword of RAM on a machine with a megaword of
it. `unsigned char` arrays with loop-indexed access are independently
proven safe on this backend already — `examples/char_test.c` and
`examples/mandel240_view.c` (a 2400-element `unsigned int` array) both
predate this work and exercise the same pattern at larger scale.

**`pmm_free_page()` on an out-of-range `pfn` is silently ignored, not
asserted.** There is no OS panic/abort mechanism anywhere in this
project yet for a bookkeeping function to call into — see
`MMPU_NOTES.md`'s own scope notes on how far this incremental approach
has gotten (real fault *detection*, no recovery/resume). Treating a bad
`pfn` as a no-op rather than corrupting the bitmap (e.g. writing outside
the array) is the conservative choice available without that
infrastructure; a real kernel built on this would want to escalate this
into a genuine kernel panic once one exists.

## 4. Real test evidence (`examples/pmm_test.c`)

Every value below was predicted by hand from `pmm.c`'s first-fit
(lowest-free-index) policy *before* running anything, then checked
against the real transcript — this project's standing rule
(`MMPU_NOTES.md`'s own recurring "falsifiable prediction, then confirm"
pattern) applied to the allocator instead of the MMPU primitives
directly.

**Compiled through the real, full `eclipse-cc` pipeline** (not
hand-assembled):
```
$ PATH="$HOME/dev/dgasm-src:$PATH" \
  ./eclipse-toolchain/eclipse-cc -o /tmp/pmm_test.simh \
  examples/mmpu.c examples/pmm.c examples/pmm_test.c
```

**Run for real on `~/dev/simh-src/BIN/eclipse`**:
```
$ { cat /tmp/pmm_test.simh; echo 'dep PC 50'; echo 'run 50'; echo 'quit'; } \
  | ~/dev/simh-src/BIN/eclipse
```

Actual output (unedited):
```
free_at_start=992
pfn0=32 pfn1=33 pfn2=34 pfn3=35 pfn4=36 pfn5=37
free_after_6allocs=986
before_free[0]=1000
before_free[1]=1001
before_free[2]=1002
before_free[3]=1003
before_free[4]=1004
before_free[5]=1005
free_after_2frees=988
realloc_a=33 realloc_b=35
free_after_2reallocs=986
readback_a=5001 readback_b=5002
still0=1000 still2=1002 still4=1004 still5=1005
drained=986 free_after_drain=0
over_alloc=-1 free_after_overalloc_attempt=0
free_after_1free=1
recovered=33 free_final=0

HALT instruction, PC: 00057 (LDA 2,@2,3)
```

**Every printed value matches the hand-worked prediction exactly**,
covering all of the task's required behaviors in one real run:

- **Real physical memory, not just bitmap state**: `pmm_alloc_page()`
  hands out frames 32-37; each gets a *distinct* pattern (`1000+i`)
  written via `mmpu_write_far()` and reads back correctly via
  `mmpu_read_far()` — proof these pfns are real, independently
  addressable physical words reachable only past the 32768-word logical
  ceiling, the same style of proof `mmpu_far_multi_test.c` established
  for hand-picked addresses, now driven by the allocator's own output
  instead.
- **First-fit / lowest-free-index behavior, confirmed**: freeing frames
  33 and 35 (`pfns[1]`/`pfns[3]`) then allocating two more returns
  exactly 33 then 35 back — the freed frames, not new ones — matching
  the linear-scan design in `pmm_alloc_page()`.
- **No cross-contamination between reused and still-live frames**:
  after overwriting frames 33/35 with new data (5001/5002), frames
  32/34/36/37 (`still0`/`still2`/`still4`/`still5`) — still allocated,
  never freed — read back their *original* 1000/1002/1004/1005 values
  exactly, unchanged by the reallocation and rewrite of their
  neighbors.
- **Exhaustion, handled cleanly, not just assumed**: draining every
  remaining frame (`drained=986`, matching `PMM_NUM_FRAMES(992) - 6`
  already-allocated exactly) brings free count to 0; a further
  `pmm_alloc_page()` call past that point returns `PMM_NONE` (`-1`) —
  the documented sentinel, not a garbage frame number, not a crash —
  and, critically, **the free count is unchanged by the failed call**
  (`free_after_overalloc_attempt=0`, same as just before it), directly
  confirming the failed allocation attempt didn't corrupt allocator
  state. Freeing one frame afterward immediately makes the allocator
  usable again (`recovered=33`, the frame just freed, `free_final=0`
  after that last successful alloc) — full recovery from exhaustion,
  not a permanently wedged allocator.

**Independent secondary confirmation**, the same "trace/dump plus
program output" double-check style `MMPU_NOTES.md` uses throughout —
direct physical memory examination via SIMH's own `e` command after the
same run, checked against the program's own printed values:
```
$ { cat /tmp/pmm_test.simh; echo 'dep PC 50'; echo 'run 50'; \
    echo 'e 100000'; echo 'e 102000'; echo 'quit'; } | ~/dev/simh-src/BIN/eclipse
100000:	001750
102000:	011611
```
`100000` octal = physical page 32 (`040<<10`), offset 0 — `001750` octal
= 1000 decimal, matching `still0=1000` (frame 32 was never freed, and
its original write survives). `102000` octal = physical page 33
(`041<<10`), offset 0 — `011611` octal = 5001 decimal, matching
`readback_a=5001` (frame 33 was freed and reallocated, and holds the
*new* data, not the old 1000+1=1001). Both computed by hand from the
program's own reported pfns before running the `e` commands, not
reverse-engineered from the result.

## 5. Regression check

Zero existing files modified — only `examples/pmm.h`, `examples/pmm.c`,
`examples/pmm_test.c`, and this document are new, so nothing else is
affected by construction. Re-run anyway, per this project's standing
practice:
- `dgasm-src` CTest: **315 tests, 314 passing, 1 Not Run**
  (`memcheck_hello`, `valgrind` still not installed) — identical to
  every prior phase's baseline.
- `examples/sizeof_check.c` through the full `eclipse-cc` pipeline:
  byte-identical (`1 2 2 4 2 4 10 14`).

## What this doesn't cover

- **No integration with a real scheduler/process model** — there is
  none yet anywhere in this project (`MMPU_NOTES.md`'s own standing
  note). This allocator is the physical-frame bookkeeping primitive a
  future one would call into, nothing more.
- **No virtual-memory-style per-process page tables** — handing out a
  physical frame is only half of what a real process would need; making
  it *usable* by a process still means programming one of the two real
  user maps (A/B) to point at it, the same hardware ceiling
  `MMPU_NOTES.md` already flagged, called out again above under
  "Real constraint worth flagging."
- **No higher-level allocation sizes** — this hands out exactly one
  1024-word frame at a time; anything needing contiguous multi-frame
  regions (e.g. a large buffer spanning more than 1024 words) needs a
  caller-side loop over multiple `pmm_alloc_page()` calls with no
  guarantee of physical contiguity between them — this allocator makes
  no attempt at contiguous multi-frame allocation, deliberately, to
  keep this increment's scope matched to what was actually asked for
  (single-frame alloc/free) and verifiable the same rigorous way as
  everything else in this project.
