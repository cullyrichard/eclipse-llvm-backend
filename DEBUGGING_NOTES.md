# `test_fps_add.c` — Debugging Notes

## Background

`examples/test_fps_add.c` is a C conversion of a hand-assembled program
(`test_fps_add.asm`, from https://github.com/cullyrichard/fps) that drives
an FPS100 floating-point coprocessor board — a Data General peripheral at
device code `054`, wired to the Eclipse via a two-register command
protocol (`DOA` writes a "switch register" or "function register" command,
`DOB` writes the accompanying data word, `DIB` reads a result word — all
*bare*, no Start/Clear pulse, no Busy/Done wait loop, unlike every other
device this project talks to).

**Important fact, already confirmed**: the original hand-assembled
`.asm` genuinely works on the real machine. So the FPS100 protocol itself,
the 56-word microprogram it loads, and the constants it references
(`!THIRD`, `!SIXTN` — which live *inside* the FPS100's own internal table
memory, not Eclipse core memory, so nothing needs to pre-load them) are
all known-good. Any remaining bug is in the C conversion or the compiler,
not the hardware protocol.

## What the program does

1. Reset the FPU (`NIOP 054`), set it busy (`NIOS 054`).
2. Load a start address into TMA (`cmd_wtsr` + `fn_load_tma`).
3. Load a 56-word microprogram, 4 sub-words at a time, cycling through
   `fn_load_ps_0..3` (a loop — `for (i = 0; i < 56; i += 4) { ... }`).
4. Start the FPU running at the loaded address (`cmd_wtsr` + `fn_start`).
5. Poll (`cmd_rdfn` via `DIB`) until the `fn_stop` bit is set
   (`waitstop` in the original).
6. Read back 5 result registers and print each via `printf("%o\n", ...)`
   (the original halted after each read for manual front-panel
   inspection — that's the one deliberate behavior change from the
   original, per the request that started this conversion).

## Bugs found and fixed, in the order we found them

Each of these was independently verified — see the exact commands below
if you want to reproduce the verification yourself.

### 1. Stale interrupt state (fixed, but not the root cause)

`eclipseemu` always starts from a clean slate (Interrupt On = 0, nothing
at memory address 1). Real hardware doesn't reset that between program
loads — if something else run on the machine earlier left interrupts
enabled with a vector pointing at a now-irrelevant handler, a real
hardware interrupt firing mid-program would jump through that stale
vector into this program's unrelated memory layout. Fixed by adding
`IO_PULSE_CLEAR(077)` (`NIOC 077`, INTDS) as the very first thing `main`
does. **This did not change the observed symptom on real hardware**, so
it either wasn't the actual cause, or wasn't the only one — but it's a
correct, worthwhile defensive fix regardless and should stay.

### 2. Function-call overhead changing device timing (fixed, real progress)

The original `.asm` writes to the FPU with tight, fixed `LDA/LDA/DOA/DOB`
blocks — almost no elapsed time between successive device operations.
The first C conversion used real *functions* (`fpu_out`/`fpu_in`, called
via `JSR`), which added a full prologue/epilogue/return around every
single device operation — dramatically more elapsed time between
operations than the original ever had. `eclipseemu` can't confirm or
deny whether this matters, since it doesn't simulate device `054` at all.

**Fix**: converted `fpu_out`/`fpu_in` from functions to macros, so every
call expands inline at its call site with no `JSR` at all. Verified via
generated assembly: 0 occurrences of `fpu_out_SLOT`/`fpu_in_SLOT` (the
indirect-call jump table entries) after the change, and each call site
now compiles to exactly `LDA/LDA/DOA/DOB`, matching the original's
cadence closely.

**This visibly changed behavior** — before this fix, the program hung
before even completing the load sequence (PC ended up parked at address
0, the interrupt-return-save location — see notes on that below). After
this fix, loading completes and the FPU visibly starts running. So this
was very likely a real, load-bearing fix, even though it didn't fully
resolve the issue.

### 3. Constant-pool bug in the `waitstop` loop (fixed, but not the root cause either)

Found by directly inspecting the generated assembly's constant pool: the
`waitstop` loop's condition (`status & fn_stop`) was loading `CPI0_2 =
128` (which is actually `load_addr`) instead of the correct `fn_stop =
32768` (`0100000` octal). Confirmed real via:

```bash
clang -cc1 -triple eclipse-dg-none -S -o test.s test_fps_add.c
grep -n 'var CPI0_' test.s   # cross-reference against expected values
```

Confirmed this is *not* a general boundary-value bug (an isolated
single-constant test correctly materializes `32768`/`0x8000` on its
own) — it's specific to referencing a constant from inside a loop
back-edge, in a function with an unusually large number of distinct
constant-pool entries (`main` here has ~23, more than anything else
built in this project so far).

**Fix**: hoisted `fn_stop` into a local variable (`stop_bit`), loaded
once before the loop, so the loop condition reads a stable stack slot
instead of re-referencing the buggy constant-pool site each iteration.
Verified the fix produces the correct value at the actual use site.

**We re-verified the entire `loadpg` (loading) loop for the same bug
class** — it has the identical loop-back-edge shape — and found it
completely correct: all 8 constants used across the loop's 4 sub-word
blocks (`cmd_wtsr`, `cmd_wtfn`, `fn_load_ps_0..3`, and the two array
byte-offset constants) match their expected values exactly. So this
specific bug class does not appear to be present anywhere else in the
loading path.

**This also did not change the observed symptom** — after this fix, the
report was "same results": the AP is running but never stops.

### 4. `LEAGA` ignored constant array-index offsets (fixed)

Found the same way as everything else in this file: single-stepping the
whole 56-word program-load sequence on `eclipseemu` and diffing every
value actually sent to the FPU (the `DOB` data word for each `fpu_out`
call — 116 values total across the load loop) against the known-correct
expected data from the original `.asm`. Also useful background: this
backend materializes a large global's address indirectly — a page-zero
`_PTR` word holds the real address, and `LEAGA` (`llvm/lib/Target/Eclipse/
EclipseAsmPrinter.cpp`, `emitInstruction`'s `LEAGA` case) is the
instruction that loads it — because these globals are too big to live
directly in page-zero themselves.

**The bug**: `LEAGA` completely ignored any offset on its `GlobalAddress`
machine operand. `opt` folds a compile-time-constant `getelementptr(@global,
N)` — e.g. indexing `fpu_program[N]` for a literal N, or one the optimizer
proved constant, which is exactly what `-passes=internalize,globaldce`
does to this kind of code — directly into that offset field rather than
leaving it as a separate runtime `ADD`. Since `LEAGA` never read the
offset at all, `fpu_program[N]` for any N != 0 silently loaded
`fpu_program[0]`'s address instead — every indexed access past the first
element quietly read whatever unrelated data happened to follow it in
memory.

**Fix** (`EclipseAsmPrinter.cpp`'s `LEAGA` case, `EclipseAsmPrinter.h`):

1. Actually read `MI->getOperand(1).getOffset()`.
2. Divide by 2 — the offset LLVM hands back is a *byte* offset, but this
   backend's addresses are word-granular (every Eclipse word is 2 of
   those `DataLayout` bytes — the same reason
   `EclipseFrameLowering::processFunctionBeforeFrameFinalized` divides
   object sizes by 2). Using the raw byte count landed exactly 2x too
   far — confirmed by tracing the actual deposited memory addresses in a
   generated `.simh` and finding "base + 10" was reading element 10, not
   element 5.
3. Materialize the (now word-granular) offset as its own page-zero
   constant word — this ISA has no immediate-operand `ADD` — and add it
   into the loaded base pointer, using `AC2` as temporary scratch
   (saved/restored around it). This mirrors how `MULrr`/`UDIVrr`'s
   post-RA expansion in `EclipseInstrInfo.cpp` already repurposes `AC2`
   for the hardware MUL/DIV operand convention; it's safe here for the
   same reason — `LEAGA`'s destination register class excludes the
   reserved `AC2`/`AC3`, so this can never save/restore the wrong
   register.

Also touched: `EclipseAsmPrinter.h` (new `OffsetSlots` bookkeeping vector
+ `addOffsetSlot` helper, mirroring the existing `AddrSlots`/`addAddrSlot`
pattern used for the `_PTR` slots themselves), and
`eclipse-toolchain/reorder_asm.py`'s `PZ_VAR_RE` regex, which needed to
additionally recognize the new `*_offN` symbol naming so these new
constant words get placed in page-zero alongside every other
pointer/constant slot (`LDA` defaults to page-zero addressing — a slot
placed outside page-zero would silently be unreachable the same way the
original bug was silent).

**Verified**: re-ran the full single-step/diff comparison after the fix —
all 116 values sent to the FPU device across the entire 56-word load
sequence now exactly match expected data (0 mismatches, versus
mismatches on every `fpu_program[N]`, N != 0 access before the fix).

### 5. Runtime-variable array index — same byte/word mismatch, generic-arithmetic version (fixed)

The `LEAGA`-offset fix (bug #4) only covers the case where the array
index is a *compile-time constant* that `opt` has already folded into the
`GlobalAddress` operand's offset field. A genuine *runtime-variable*
index — e.g. `fpu_program[i+k]` inside a `for` loop where `i` is a loop
counter, which is what the *original*, non-unrolled version of the load
loop in `test_fps_add.c` looked like — takes a completely different code
path: LLVM computes the address with real runtime `ADD` instructions on
top of a **zero-offset** `LEAGA` (i.e. just the base pointer), not via a
nonzero `LEAGA` offset at all. So bug #4's fix never triggers for it.

**Confirmed** (before the fix): rewriting the load loop as a real `for`
loop and re-running the same single-step/diff-against-`eclipseemu`
verification used for bug #4 showed **42 of the 116** DOB values sent to
the FPU were wrong.

**Root cause**: the same byte-vs-word unit mismatch as bug #4, just
reached through generic arithmetic instead of `LEAGA`'s offset field.
Standard, target-independent GEP lowering always scales a runtime array
index by the element type's `DataLayout` size in *bytes* (2, for `i16`/
`i32` — the only element types this backend's array/pointer access
supports). That byte-scaled offset then gets added, via a plain `ADD`,
directly to the *word*-granular base pointer `LEAGA` materializes —
landing exactly twice as far as it should (index 4 resolves to element
8, etc.). Confirmed precisely via single-step tracing a minimal 16-word
reproduction: `AC0 = 2*i` (self-doubling `ADD 0,0`), then
`AC1 = fpu_program_PTR_value + AC0` with no division — the identical
defect class as bug #4, in a different code path.

One earlier false trail worth recording: an initial bulk-verification
pass appeared to show the *first* sub-word access of each 4-word group
(`fpu_program[i+0]`) was always correct, suggesting the bug was specific
to combining a runtime index with an *additional* constant offset. This
turned out to be a coincidence of the specific test data used (a
hand-crafted minimal array where element 4 and element 8 happened to
hold the same value, masking a real "reads element `2*i` instead of
element `i`" defect at `i=4`) — the defect actually affects every
runtime-indexed sub-word uniformly, including `+0`.

**Fix** (`EclipseISelLowering.h`/`.cpp`, `EclipseInstrInfo.td`): a
target-specific DAG combine, `EclipseTargetLowering::PerformDAGCombine`,
hooked on `ISD::ADD`. When one operand is a `WRAPPER`'d `GlobalAddress`
(i.e. a `LEAGA`-backed base pointer) and the other is a non-constant
runtime offset, the combine halves that offset before letting it combine
with the base pointer.

Two dead ends hit and fixed along the way, both confirmed via
`llc -debug-only=dagcombine` showing the exact infinite loop:

1. Returning a plain `ADD` (guarded by an "is the offset already halved"
   heuristic check) instead of a distinct opcode: DAGCombiner's own
   canonicalization re-shapes nodes between visits, defeating the
   heuristic and looping forever. Fixed by introducing a dedicated
   `EclipseISD::WORD_ADD` node for the combine's output — structurally
   unable to match the same `N->getOpcode() == ISD::ADD` check again —
   with a `Pat` selecting it to the existing `ADDrr` instruction.
2. Building the halving from a plain `ISD::SRL`/`ISD::UDIV`: `SRL` is
   only ever `Custom`-lowered here (via `LowerShift`, into
   `UDIV(x, 2)`), and DAGCombiner's own standard "divide by a
   power-of-two constant → shift" fold immediately turns that back into
   `SRL(x, 1)` — since `SRL` is never strictly `Legal`, each generated
   `SRL` re-enters `LowerShift` and the two folds ping-pong forever.
   Fixed the same way: a dedicated `EclipseISD::HALVE` node, invisible to
   both generic rules, selecting straight to the existing, already-proven
   `UDIVrr` pseudo.

(Also worth knowing if you're extending this: a `Pat` accidentally placed
inside the pre-existing `let Defs = [AC2] in { ... }` block around
`MULrr`/`UDIVrr`/`UREMrr` fails TableGen with a cryptic "Value 'Defs'
unknown!" — `Pat` isn't an `Instruction` subclass and has no `Defs`
field. Keep any new `Pat` outside that block.)

**Verified**: re-ran the full single-step/diff comparison with the load
loop restored to a real nested `for` loop (matching the original `.asm`'s
`loadpg` shape) — all 116 values sent to the FPU across the entire
56-word load sequence exactly match expected data (0 mismatches). Also
re-verified on the original minimal 16-word isolated reproduction (0/32
mismatches, including the previously-miscategorized `+0` sub-word case).

### 6. Register scavenger crash — no emergency spill slot reserved (fixed)

Found on a different test program: `examples/char_test.c`, a local
`char arr[] = {'H','e','l','l','o'}` printed one character at a time in a
loop, which crashed outright — `LLVM ERROR: Cannot scavenge register
without an emergency spill slot!` — before ever reaching the assembly
printer. `test_fps_add.c` and everything else in this package up to now
happened to never have enough register pressure mid-function to hit this
path, which is exactly why it went unnoticed for this long.

**The bug**: `EclipseRegisterInfo::requiresRegisterScavenging` and
`requiresFrameIndexScavenging` (`EclipseRegisterInfo.h`) both
unconditionally return `true` — needed because `LEAFI`'s post-regalloc
expansion (`eliminateFrameIndex`) sometimes needs a genuine scratch
physical register it can't otherwise conjure up. That's a real
requirement, not a mistake. But returning `true` is also the only thing
that tells PEI (`PrologEpilogInserter`) to build a `RegScavenger` for
this function in the first place, and a scavenger can only manufacture a
free register by spilling whatever currently occupies it *somewhere* —
which means it needs a reserved stack slot to spill into, registered via
`RS->addScavengingFrameIndex(...)`. Nothing in
`EclipseFrameLowering.cpp` ever called that. So the scavenger existed,
was told it was allowed to be needed, and had nowhere to put anything the
moment a function actually forced a mid-function scavenge — a local
array plus a loop plus call-heavy code (`printf` in a loop) was enough
register pressure to trigger it.

**Fix** (`EclipseFrameLowering.cpp`,
`EclipseFrameLowering::processFunctionBeforeFrameFinalized`): reserve a
dedicated one-word emergency spill slot up front and register it with
the scavenger, right at the top of the function, before anything else
runs:

```cpp
int EmergencyFI = MFI.CreateStackObject(2, Align(2), /*isSpillSlot=*/true);
RS->addScavengingFrameIndex(EmergencyFI);
```

This also required changing the (previously unused and therefore
anonymous) `RegScavenger * /*RS*/` parameter back to a named `RegScavenger
*RS`, and adding `#include "llvm/CodeGen/RegisterScavenging.h"` for
`addScavengingFrameIndex`'s declaration. One word is enough — nothing on
this target ever needs to scavenge more than one register at a time.

**Verified**: `examples/char_test.c` now compiles and runs to completion
under `eclipseemu` instead of crashing in `llc`. Also re-checked with a
second, independently-triggering repro (`int arr[] = {1,2,3,4,5};`
printed in a loop) — previously crashing the same way, now compiles and
prints `1 2 3 4 5` correctly.

### 7. `LEAGA` divided string-global offsets by the wrong unit (fixed)

Fixing bug #6 got `char_test.c` past the crash and onto the emulator —
where it ran, but printed the wrong characters instead of `Hello`.

**The bug**: bug #4 (above) taught `LEAGA` (`EclipseAsmPrinter.cpp`,
`emitInstruction`'s `LEAGA` case) to divide a `GlobalAddress` operand's
byte offset by 2 before using it, to convert LLVM's byte-granular offset
into this backend's word-granular addressing. That's correct for the
case bug #4 was fixed against — a numeric array, which
`emitGlobalVariable` emits as one full word per element regardless of
the element's own `DataLayout` size, so 2 `DataLayout` bytes of offset
really does mean 1 word of address. It is *not* correct for string
literals. `emitGlobalVariable`'s string-literal path emits dgasm's plain
`var name = "..."` form (no `packed` suffix) — and dgasm (the real
assembler; see its `assembler.c`, `VARIABLE_STRING` vs
`VARIABLE_PACKED_STRING`) reserves one full word *per character* for
that form, not two characters per word. So for a string global, a byte
offset is *already* a word offset, and the unconditional `/ 2` truncates
every odd byte offset down onto its even neighbor's word — bytes 2 and 3
both landing on word 1, and so on — silently reading (or writing) the
wrong character on every odd index.

**Fix** (`EclipseAsmPrinter.cpp`, same `LEAGA` case): before dividing,
check whether the target global's initializer is a string
`ConstantDataArray`:

```cpp
int64_t Divisor = 2;
if (const auto *GV = dyn_cast<GlobalVariable>(MI->getOperand(1).getGlobal())) {
  if (GV->hasInitializer()) {
    if (const auto *CDA = dyn_cast<ConstantDataArray>(GV->getInitializer()))
      if (CDA->isString())
        Divisor = 1;
  }
}
int64_t Offset = MI->getOperand(1).getOffset() / Divisor;
```

Needed `#include "llvm/IR/GlobalVariable.h"` for the `dyn_cast`. Numeric
arrays keep the existing (correct) `/ 2` behavior; string globals now use
`/ 1`.

**Verified**: confirmed the divisor selection against both global
shapes — string globals now resolve each character's address correctly
regardless of odd/even offset; re-checked bug #4/#5's numeric-array
`fpu_program` indexing is unaffected (still divisor 2, still 0/116
mismatches in the `test_fps_add.c` regression pass).

### 8. `memcpy` lowering coalesced unaligned bytes into a meaningless word (fixed)

Even with bugs #6 and #7 fixed, `char_test.c` still printed the wrong
letters. `arr`'s *initializer copy itself* — the compiler-generated
`llvm.memcpy` that clang emits to copy `char arr[] = {'H','e','l','l','o'}`'s
constant data onto the stack — turned out to be corrupting the data
before `LEAGA` or anything else ever got involved.

**The bug**: for a 1-byte-aligned, 5-byte `memcpy`, LLVM's generic
memcpy expansion picks i16-sized chunks where it judges that profitable,
then — since the source isn't actually 2-byte aligned — expands each i16
chunk into two i8 sub-loads combined with a shift and an `OR`. That's a
standard, correct technique on a real byte-addressed machine emulating
an unaligned wide access. This target isn't byte-addressed: nothing in
this backend ever packs two `i8`s into one word (bug #7, just above,
is direct confirmation of that — string data gets one full word per
character, never two-per-word). So the "combined" 16-bit value from
shift-and-OR-ing two unrelated one-word characters together corresponds
to nothing real, and storing it back out corrupts both characters.

**Fix** (`EclipseISelLowering.h`, in the `EclipseTargetLowering` class
body): override `TargetLowering::getOptimalMemOpType` to force byte-sized
(`i8`) chunks whenever the copy isn't genuinely 2-byte-aligned, and fall
through to the target-independent default otherwise:

```cpp
EVT getOptimalMemOpType(LLVMContext &Context, const MemOp &Op,
                         const AttributeList &FuncAttributes) const override {
  return Op.isAligned(Align(2)) ? MVT::Other : MVT::i8;
}
```

An earlier attempt forced `i8` unconditionally, which fixed
`char_test.c` but broke the already-working `int arr[] = {1,2,3,4,5};`
case: that copy is 10 bytes and genuinely 2-byte-aligned, and forcing
single-byte chunks pushed its chunk count over
`getMaxStoresPerMemcpy`'s limit, so LLVM fell back to a `memcpy` libcall
— which this freestanding target has no way to lower, and which
crashed for real in `LowerCall`. Gating on `Op.isAligned(Align(2))`
avoids that: aligned numeric-array copies still get the generic (and
correct) i16 chunking, and only genuinely unaligned char/string copies
get forced down to i8.

**Verified**: re-ran `char_test.c` under `eclipseemu` after this fix —
prints `Hello` one letter per line, correctly, for the first time.
Re-verified the `int arr[] = {1,2,3,4,5};` case in the same pass — still
compiles (no libcall fallback) and still prints `1 2 3 4 5` correctly.
Also re-ran the full existing regression pass to make sure none of
bugs #6–#8 disturbed anything already verified: `test_fps_add.c` still
shows 0/116 mismatches against `eclipseemu`, and the minimal `loop_test.c`
repro from bug #5 still shows 0/32 mismatches.

## RESOLVED: the "regmd always reads 15" / waitstop-never-signals bug

**This is fixed.** Root cause: a genuine, previously-undocumented
compiler bug, found by adding a `printf("status: %o\n", status)` right
after the `waitstop` loop exited — the printed value was mathematically
inconsistent with the loop's own exit condition, which is what proved
this was a compiler bug rather than a hardware/timing issue (ruling out
the three hypotheses at the end of the "Current state" section below).

**The actual bug**: unsigned comparisons (`>=`, `<`, etc. on `unsigned
int`) were being lowered through the same sign-bit-test machinery as
*signed* comparisons. That's wrong for any unsigned value whose sign bit
happens to be set (≥ 32768 unsigned) — the difference's sign bit doesn't
reliably indicate the unsigned relation. Confirmed via direct,
step-by-step `eclipseemu` tracing with hand-picked operand values before
touching any code.

Properly fixing this (rather than patching just the one call site) also
required:
- A native-carry-based unsigned comparison (`BGEU`/`BLTU`), since the
  sign-bit trick genuinely doesn't work for unsigned values — this in
  turn needed the correct Nova/Eclipse `SZC`/`SNC` skip-letter polarity,
  which turned out to be the *opposite* of what the mnemonics suggest
  (confirmed with a minimal, isolated single-instruction test: deposit
  the carry flag directly, execute nothing but the skip instruction,
  observe whether PC advances by one word or two).
- A separate `LowerShift` bug this surfaced: right-shift-by-N was
  chained as N separate divide-by-2 calls sharing one operand, and the
  hardware `DIV` instruction's own side effect (clobbering the register
  holding that shared operand) silently corrupted the chain from the
  second division onward. Fixed by computing a single divide by `2^N`
  instead of chaining N divides by 2.
- The unsigned-comparison instruction itself (`SUB#`, in test mode so it
  doesn't clobber either operand) turned out to have an *unforceable*
  carry on real Nova/Eclipse hardware — its carry field is "none",
  meaning the carry it leaves behind is whatever was already there
  before the instruction, unrelated to the operands. Confirmed via a
  direct carry-in/carry-out truth table on `eclipseemu`. The real fix
  needed `SUBZ` (forced carry-in), which can't run in test mode — so it
  borrows AC2 (the frame pointer) as scratch, save/restore via the
  software stack, mirroring the existing MUL/DIV pseudo-expansion
  pattern in this same backend.

Also unrelated but found and fixed along the way, while chasing this:
real Eclipse hardware terminals don't return to column 0 on a bare LF —
without an explicit CR first, each `printf`/`putchar('\n')` output drifts
one line further right each time ("ever growing spaces" staircase),
confirmed on real hardware and not reproducible on `eclipseemu` (whose
simulated terminal already does LF→CRLF translation itself, which is
exactly what made this invisible in all the `eclipseemu`-only testing
that led up to this point). Fixed in `eclipse_rt.c`'s `putchar`.

With all of the above fixed, `examples/test_fps_add.c` was re-verified
on real hardware and works correctly end to end.

## Historical investigation (kept for context — the fix above is current)

This section is about a separate, older issue (a real-hardware-only
symptom) from bug #5 above (a compiler bug reproducible on `eclipseemu`
alone, now fixed). `examples/test_fps_add.c` in this package now uses a
real nested `for` loop to load the microprogram (bug #5's fix is
verified: 0/116 mismatches), so this section's hang is independent of
that bug either way.

As of the last real-hardware test: **the FPU visibly starts running
(progress from the earlier hard hang), but never signals "stop."** The
`waitstop` condition itself is now verified correct against the actual
generated code, and the loading sequence is verified correct end to end
(all 56 data words are byte-identical to the known-working original —
confirmed via `diff -w` after accounting for a CRLF-vs-LF artifact in the
original file — and the load loop's logic and constants all check out).

We do not yet know whether this means:

- The AP genuinely hasn't finished its computation yet (needs more time,
  or the wait was interrupted before completion), or
- The AP has actually finished, but something is *still* wrong with how
  status is being read/interpreted (a second instance of the constant-
  pool bug we haven't found, or something else), or
- There's a genuine hardware/timing issue with this device that has
  nothing to do with the C conversion.

**What would help most in narrowing this down:**

1. A way to read the AP's status register directly and independently of
   what this program computes — to establish ground truth on whether the
   stop bit is actually set.
2. Knowing how long the original `.asm` took to go from start to stop on
   a known-good run, to know what "still running" should look like.
3. If you find another wrong-constant instance, the fastest way to check
   is exactly the technique used above: `clang -cc1 -S` the file, `grep
   -n 'var CPI0_'`, and cross-reference every `LDA n,CPI0_N` against what
   C-level constant it's supposed to correspond to, in source order.

## Reproducing the eclipseemu-only verification steps

None of these require real hardware or a working FPU simulation — they
only check compiler/CPU-side correctness, which is exactly what's been
useful so far:

```bash
# Compile and inspect generated assembly
clang -cc1 -triple eclipse-dg-none -S -I eclipse-toolchain/rt/include \
  -o test.s examples/test_fps_add.c

# Full pipeline build (what actually gets loaded onto the machine)
./eclipse-toolchain/eclipse-cc -o test.simh examples/test_fps_add.c

# Run on eclipseemu -- will hang in the waitstop poll since eclipseemu
# doesn't simulate device 054, but everything up to that point (the
# entire load sequence) runs to completion and can be inspected:
{ cat test.simh; echo 'dep PC 100'; echo 'step 100000'; echo 'e PC'; \
  echo 'quit'; } | eclipseemu
```
