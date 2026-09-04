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

### 9. Runtime-variable array index through a loaded *pointer parameter* — same defect, third code path (fixed)

Bugs #4 and #5 cover a compile-time-constant index into a global
(fixed via `LEAGA`'s offset field) and a runtime index into a global
(fixed via the `PerformDAGCombine` hook on `ISD::ADD`, recognizing a
`WRAPPER`'d `GlobalAddress` base). Neither covers a third shape: a
runtime index through an array reached via a **pointer parameter** —
e.g. `load_psm(unsigned int pgm_addr, unsigned int fpu_pgm_len, const
unsigned int fpu_pgm[])`, indexing `fpu_pgm[i+k]` inside its own load
loop, once that loop lives in a real out-of-line function instead of
being inlined at every call site.

**Confirmed** (before the fix, via a minimal repro — a function taking
`(int len, const unsigned int arr[])`, looping `for (i = 0; i < len;
i++) printf("%d ", arr[i])`): printed `arr[0], arr[2], arr[4], ...`
instead of `arr[0], arr[1], arr[2], ...` — the array index silently
doubling, then running off the end of the array into adjacent memory
once `i` exceeded half the real length.

**Root cause**: the same byte-vs-word unit mismatch as bugs #4/#5, one
base-pointer shape further removed. `PerformDAGCombine`'s operand
detection only matches a `WRAPPER`'d `GlobalAddress` (a global) or a
bare `ISD::FrameIndex` (a locally-declared array's own address) as the
"this is a word-granular base pointer" signal. A pointer *parameter*'s
value is neither — this backend reloads such values from their own
stack slot at every use rather than keeping them live in a register
across statement boundaries, so by the time the array-index `ADD`
reaches this combine, the base operand is a plain `ISD::LOAD` — a shape
the existing checks never considered, so the byte-scaled runtime offset
never got halved.

**Fix** (`EclipseISelLowering.cpp`, `PerformDAGCombine`): recognize a
third case — exactly one of the `ADD`'s two operands is an `ISD::LOAD`
— but only when this `ADD`'s own result is used directly as a
load/store's base-pointer operand (confirmed against
`EclipseISelDAGToDAG.cpp`'s `ISD::LOAD`/`ISD::STORE` handling: a
non-`FrameIndex` base falls through completely unchanged to the generic
`LDIND`/`STIND` pattern, so this `ADD` is still exactly that operand at
the point the combine runs). The usage check matters: plenty of
ordinary integer arithmetic adds two loaded values together with no
pointer involved at all (`a[i] = b[j] + c[k]`), and halving one side of
that blind would silently corrupt it. Gating on "feeds a memory access"
is both necessary and sufficient to catch the parameter case without
misfiring on unrelated arithmetic.

Also added, in the same combine: when the offset being halved is
already exactly `X+X` (this backend's `LowerShift` custom-lowers `x <<
1` — how a byte-scale-by-2 usually reaches this point — into a literal
self-add, not a `MUL`), skip the runtime `HALVE`/`UDIVrr` round trip
entirely and use `X` directly. Doubling a value and then dividing it
back by 2 via a real division instruction is pure waste; on a large
enough program the extra instructions across every array access were
enough to push a branch target past `dgasm`'s 0–255 page-relative `JMP`
range, breaking compilation of an otherwise-unrelated file. This
optimization applies to all three base-pointer cases, not just the new
one.

**Verified**: the minimal repro above now prints `0 1 2 3 ... 9` in
order. Re-ran the full existing regression pass (package examples +
every minimal repro built while isolating this bug) to confirm nothing
else moved: no change in behavior anywhere else in the suite.

### 10. Struct-typed globals lost every field past the first (fixed)

Found via a standalone reproduction (`clobber.c`) built around the same
`fps_word_struct { int exp, mh, ml; }` shape `fps.c`'s `read_md` returns:
a `static fps_word_struct mainData;` global, written by value from a
function's return and read back by field in a loop interleaved with
`printf`.

**Confirmed**: `mainData.mh`/`.ml` came back reading garbage — traced to
the exact bytes of `"md"`, the first two characters of the *format
string* laid out immediately after `mainData` in page-zero. The actual
*values* computed and returned by the function were correct at every
call site (checked directly in the generated IR/assembly) — this was
purely a storage-size bug, not a codegen bug in whatever computed the
struct's contents.

**Root cause**: `EclipseAsmPrinter::emitGlobalVariable` had cases for a
scalar, a string literal, and a numeric array initializer, each emitting
the right number of `var` words — but no case at all for a struct
initializer (`ConstantStruct`/a struct-typed `ConstantAggregateZero`).
Every one fell through to the same single-placeholder-word case used for
genuinely unsupported types (`var NAME = 0 // TODO: unsupported
initializer`), regardless of how many fields the struct actually had.
Since dgasm has no linker/sections — every `var` line just claims the
next sequential word — a 3-word struct with only 1 word actually
reserved for it meant the *next* global's `var` lines landed inside what
C code still believed was `mainData`'s own storage, and `mainData`'s own
later fields landed inside whatever came after that.

**Fix** (`EclipseAsmPrinter.cpp`): a recursive `flattenConstant` helper
that walks a struct's fields (or a zero-initialized struct type's
element types) in order, emitting the exact same "each element gets a
`var` line, only the very first keeps the real symbol name" convention
`emitArrayElements` already established for arrays — recursing into any
field that's itself a nested struct or array, so C-level nesting depth
doesn't matter, only the fully-flattened word sequence dgasm actually
needs. A field of a type genuinely unsupported here (a pointer, a float)
degrades to a single zero word for just that field, matching this
backend's existing "unsupported" fallback, rather than losing the fields
around it.

One bug caught in the fix itself before it shipped: the zero-initializer
recursion path calls itself with a null `Constant*` (there's no real
`Constant` object for "this field is implicitly zero" — only its
`Type`), and the first version's top-of-function `isa<>` checks crashed
outright on that null (`isa<> used on a null pointer`, confirmed via a
zero-initialized-struct repro). Fixed by checking for null explicitly
before the `isa<>` calls.

**Verified**: both a non-zero-initialized and a zero-initialized 3-field
struct global read and write every field correctly, with an adjacent
string global immediately after confirmed untouched before *and* after
writing through the struct. Re-ran the full existing regression pass
(package examples, `clobber.c` itself) — no change in behavior anywhere
else.

### 11. Replaced the software-only `_SP` calling convention with real hardware SAVE/RTN (fixed)

Not a bug fix on its own — a from-scratch replacement of how every
function call on this target works, undertaken because this backend's
own comments and this project's `README.md` carried a claim, inherited
from the sibling `eccc` project and never independently verified here:
that real Nova/Eclipse hardware `PSH`/`POP`/`SAVE`/`RTN` "were tried and
rejected as needing an OS-managed frame environment this bare-metal
target doesn't have." Direct testing found that's not true for `SAVE`/
`RTN` specifically, on `eclipseemu`'s CPU model.

**What the claim actually was protecting against**: a genuine "Nova 3
instructions" hardware option (`PSHA`/`POPA`/`SAV`/`RET`/`MTSP`/`MTFP`/
`MFSP`/`MFFP`, confirmed real by finding a virtual-instruction-emulator
trap handler for exactly this set in the DG SDK's `dgnasm_old/viemu/
trap.asm`) that traps as "Unimplemented Instruction" on machines that
don't have it — genuinely needing OS/software emulation there. But
`eclipseemu`'s CPU model (see `simh/NOVA/eclipse_cpu.c`) implements
`SAVE` (`0163710`) and `RTN` (`0127710`) *natively*, unconditionally,
with no trap involved.

**Verifying the real semantics**: `SAVE`/`RTN` use three fixed hardware
memory locations — `040` (stack pointer), `041` (frame pointer), `042`
(stack limit, checked on every `SAVE`) — plus `043` (overflow trap
vector, only touched if the limit check fails). The very first test
trapped, but only because `040`/`042` default to `0`, so any `SAVE`'s
limit check trivially failed and trapped through the unhandled vector at
`043`. Initializing both with two ordinary `STA` instructions at program
start made `SAVE`/`RTN` work perfectly — no OS, no trap handler needed.
Real semantics, confirmed against the simulator source and re-verified
against actual `eclipseemu` runs: `SAVE N` pushes AC0, AC1, AC2, old-FP,
AC3(+carry, the `JSR` return address) — 5 words, stack growing *upward*
(increment-then-store) — then reserves `N` more words *above* that;
leaves the new frame pointer in AC3 and `041`. `RTN` sets SP := FP,
reads the return address from `mem[FP]` into PC, then pops AC3(old FP),
AC2, AC1, AC0 in that order. Critically, `RTN` unconditionally restores
AC0/AC1 to their *pre-call* values — an epilogue has to overwrite those
two saved stack slots with the real return value before `RTN` runs, or
the return value is silently lost.

**The new convention** (mirrors the sibling `eccc` project's own
verified design once the false OS-dependency claim was out of the way):
prologue is `SAVE <Words>` + `MOV 3,2` (copy the new FP from AC3 into
AC2, keeping the existing "AC2 is always the frame pointer" convention
everywhere else in this backend unchanged); epilogue is `STA 0,-4,2` +
`STA 1,-3,2` (overwrite AC0/AC1's SAVE-pushed slots with the real return
value) followed by the now-implicit `RTN` (`RET`'s `AsmString` changed
from `"JMP 0,3"` to `"RTN"` — same pattern-match, same flags, nothing
else about it changed). No explicit local-deallocation loop needed:
`RTN`'s `SP := FP` discards the whole frame in one step. Argument
push/pop and a few scratch-register save/restore sites (`BGEU`/`BLTU`,
`LEAGA`'s offset-add, `MUL`/`DIV`'s AC2 save) still use a software
push/pop sequence, but now against the same fixed hardware address `040`
`SAVE`/`RTN` themselves use — not a separate `_SP` variable — so the two
can never drift apart. `_start` now initializes `040`/`042` from
`_STACKTOP`/`_STACKLIM` (renamed from the old `_SP`; `_STACKTOP` is
still dynamically rewritten by `reorder_asm.py`'s `fix_stack_pointer` to
sit safely above the program's actual footprint, same mechanism as
before, just representing the stack's *starting* point now rather than
an absolute address that never changed).

Three real bugs were found and fixed while bringing this up to full
parity with the old convention:

- **Push/pop direction mismatch.** The software push/pop sites
  (argument passing, the three scratch-save spots above) still used the
  old convention's decrement-to-reserve direction, left over from when
  the stack grew *downward*. Against the same address `040` real `SAVE`
  now grows *upward*, that direction is backwards — pushing an argument
  from inside a function's own body could collide with and corrupt that
  function's own `SAVE`-reserved frame. Caught by a single nested call
  returning the wrong value (`add(5,3)` gave `AC0=0` instead of `8`);
  fixed by flipping every one of these sites from decrement-to-reserve
  to increment-to-reserve, matching `SAVE`'s own direction, and changing
  incoming-argument addressing (`LowerFormalArguments`) from positive
  `FP+2+i` to negative `FP-5-i` to match (arguments pushed before a
  callee's own `SAVE` now land *below* the new FP, not above it).

- **`va_arg` walked the wrong direction.** Even after the fix above, a
  `printf` with two `%d` arguments printed the first correctly and read
  garbage for the second. `LowerVAARG` advanced the `va_list` pointer
  with `VAList + 1` — correct under the old downward-growing stack,
  where each next-in-source-order vararg sat at a *higher* address, but
  backwards now: with the whole stack growing upward, walking from one
  right-to-left-pushed vararg to the next (in left-to-right C order)
  means walking to progressively *lower* addresses. Fixed by changing
  the advance from `ADD` to `SUB`.

- **Ordinary locals could land on top of `SAVE`'s own pushed words.**
  The most serious of the three, and the one that made the soft-float
  runtime (`__addsf3`/`sf_add` and everything downstream) reliably
  overflow the stack and trap at `PC 0`, while simpler programs (a
  handful of sequential/nested/recursive calls) happened not to trigger
  it. `EclipseFrameLowering`'s constructor still told LLVM's generic
  frame-object-offset pass (`PrologEpilogInserter::
  calculateFrameObjectOffsets`, which silently overwrites this target's
  own offset-assignment loop in `processFunctionBeforeFrameFinalized` —
  a pre-existing, documented quirk, left alone) to grow locals
  *downward* from FP — the same side of FP as both `SAVE`'s own 5 pushed
  linkage words (`FP` through `FP-4`) and pushed arguments (`FP-5` and
  below). For a function with enough locals, or one with `long`/wide
  parameters needing extra spill room, an ordinary local could get
  assigned an offset already spoken for by one of those — confirmed
  directly: a local float constant in `main` landed at `FP-4` (AC0's
  `SAVE`-pushed slot) and was overwritten mid-function, then again by
  the epilogue's own return-value store, so by the time `RTN` read that
  slot back the real value was long gone — `RTN` restored a return
  address of `0` from a different clobbered slot and the program jumped
  straight into the trap. Root-caused by single-stepping `eclipseemu`
  through the exact failing instruction sequence and reading `AC2`/
  `mem[FP]`/`mem[FP-1..FP-4]` directly at the point of failure — all
  five read back `0`, meaning nothing legitimate had ever written them.
  Fixed by switching `EclipseFrameLowering` from `StackGrowsDown` to
  `StackGrowsUp` (`Align(2)`, `LocalAreaOffset = 2`) — locals now start
  one word *above* FP and grow upward from there, the opposite side of
  FP from both `SAVE`'s linkage words and pushed arguments, so there's
  no longer anywhere for the two to collide. `LocalAreaOffset` had to be
  `2` (one word), not `0`: offset `0` itself is `SAVE`'s own
  return-address slot, and with `LocalAreaOffset = 0` a spill/local
  object still occasionally landed exactly there (confirmed in generated
  assembly: `STA 0,0,2` in a function with `long`-typed parameters).

**Verified**: the full existing regression pass — every package example,
`clobber.c`, both struct-global tests — plus a new battery of
hand-written tests exercising what the old convention's regression pass
didn't specifically cover: sequential and nested calls, 6-level
recursion (`fact`), a 5-argument function, 32-bit `long` arguments and
return values, and multi-argument `printf`/varargs. All match established
baselines exactly. The soft-float runtime — `test_float_mul.c`,
`test_float_cmp.c`, `test_float_conv.c`, `test_float_div.c`,
`test_print_float.c` — now also matches established baselines exactly
with a clean halt, where it previously trapped or hung on every one of
them. `test_fps_add.c`/`test_fps_md.c`/`fps_dma_test.c` still hang, but
confirmed benignly: the hardware stack pointer (`040`) is stable across
a 2.5-million-step range and PC loops within a ~20-word band, consistent
with the already-documented wait on the unimplemented FPU device
(`RESOLVED` section below), not corruption.

### 12. Runtime array index into a string-literal global also got halved (fixed) — plus a related, still-open gap

Found while extending `printf`/`string.h`/`stdlib.h` with a batch of
small, previously-missing library functions (`%x`/`%u`/`%ld`/`%lu`,
`strchr`/`strrchr`/`strstr`/`strncpy`/`strncat`/`strdup`/`memcmp`,
`abs`/`labs`/`rand`/`srand`/`exit`/`strtol`/`strtoul`/`atof`) — not a
regression in anything that shipped before this.

**The bug**: `printf`'s new `%x` case is a hex-digit lookup table,
`"0123456789abcdef"[val & 15]`, exactly the array-indexed-by-a-runtime-
value shape bugs #4/#5/#9 above already fixed for *numeric* arrays.
`print_hex(255)` printed `"77"` instead of `"ff"` — both the outer and
the recursive call's `val & 15 == 15` read back element 7 (`'7'`)
instead of element 15 (`'f'`), exactly what dividing the index by 2
first would produce.

**Root cause**: `EclipseISelLowering.cpp`'s `PerformDAGCombine` HALVE
fix-up (the byte-to-word correction bugs #4/#5/#9 added, for a runtime
GEP offset added to a global/frame-index/loaded-pointer base)
unconditionally divides the offset by 2 — correct for a *numeric*
array, which dgasm packs two bytes to a word, but wrong for a
string-literal global: dgasm's plain `var name = "..."` reserves one
full word *per character*, no packing at all (already correctly
special-cased in `EclipseAsmPrinter.cpp`'s `LEAGA` handling for
*constant*-offset string indexing — see that code's own `isString()`
check — just never carried over to this earlier, runtime-offset combine).

**Fix**: when the `ADD`'s base is a `WRAPPER`'d global address (the one
case here with an inspectable `GlobalVariable` — the `FrameIndex` and
loaded-pointer cases don't have one, and correctly keep halving
unconditionally either way, since this target's frame allocator packs
every *local*, char arrays included, two bytes to a word regardless of
content), check whether it's a `ConstantDataArray` with `isString()`
true and skip the halving if so.

**A second, related bug found alongside this one — not fixed, worked
around instead**: `s[1]` (or the equivalent `*(s + 1)`) misread when
`s` is a *local pointer variable* holding a string-literal address —
confirmed with a minimal repro: `const char *s = "0x1a"; s[1]` read
back `'0'` (`s[0]`'s own value) instead of `'x'`. The *identical*
pointer, advanced with `s++` and then dereferenced bare (`*s`, no
offset in the same expression), read correctly. This is why `strtol`/
`strtoul`'s hex-prefix detection here uses a copied-and-incremented
peek pointer rather than `s[1]`, and it's also almost certainly *why*
this file's existing functions (`strlen`, `strcpy`, `strcmp`, ...) have
never once used bracket-indexing on a pointer variable, going all the
way back — apparently a lesson already learned, just never written
down anywhere until now. Root cause not yet investigated (a different
DAG shape than the one this bug entry's fix touches — likely
`EclipseISelDAGToDAG.cpp`'s custom `LOAD`/`STORE` address-mode
selection rather than `PerformDAGCombine`, since the FrameIndex/global
cases the fix above handles don't have a "load the pointer value
first" step at all). Anyone writing new code against this runtime
should stick to the same convention every existing function already
uses: advance a pointer with `++`, dereference it bare — never index
or offset-then-dereference a pointer *variable* (as opposed to a
`GlobalVariable` reached directly, which is a different, correctly-
handled DAG shape).

**Also found in the same pass, unrelated, not fixed**: 32-bit (`long`)
multiplication silently computes the wrong answer rather than failing
to compile — `1000L * 17` returned `104` (`17000` truncated mod 256,
suggesting an 8-bit-wide step somewhere in whatever this legalizes to).
Not yet root-caused. `rand()`/the new `strtol`/`strtoul` avoid it
entirely (16-bit-native LCG state; `val * base` computed as `base`
additions of `val` rather than a real multiply) rather than depending
on it.

**Verified**: full existing regression pass unchanged; `print_hex(255)`
now correctly prints `"ff"`; every new library function
(`string.h`/`stdlib.h`/`printf`'s new specifiers) checked individually
against hand-computed expected output. One real, pre-existing
constraint surfaced by this batch of additions, not a new bug: `printf`
now costs a few more words of its own always-reachable call graph
(`print_hex`/`print_long`/`print_uint32`/`u32_and_nz`, pulled in the
moment *any* program calls `printf` at all, whether it uses `%x`/`%u`/
`%l*` or not) against the same shared 256-word page-zero budget `%f`
was kept out of `printf` for entirely (see `stdio.h`'s own comment) —
confirmed a program combining `atof()` with even a bare `printf("%d",
n)` can now tip over that budget where it fit before, though `atof()`
and `printf()` individually still fit fine on their own. See
`stdio.h`'s `printf` comment for the workaround (use `putchar`/`puts`
instead of `printf` in a budget-tight program).

### 13. Two new backend gaps hit while adding math.h (worked around, neither fixed at the backend level)

Found while adding `fabsf`/`floorf`/`ceilf`/`sqrtf` — this project's first
`math.h`. Not regressions; both are things nothing before this needed.

**Gap 1: a function literally named `sqrtf` crashes outright.** LLVM's
own middle-end (inside `llc`, not clang's frontend — confirmed `-fno-
builtin`/`-fno-math-builtin` at the `clang -cc1` level make no
difference at all) recognizes any function named `sqrtf` matching
libm's signature and rewrites a call to it into a raw `FSQRT` node
before this project's own implementation is ever reached. This backend
has no libcall registered for that node, so `llc` crashes outright
("LLVM ERROR: unsupported library call operation"). `--disable-
simplify-libcalls` on `llc` does fix this specific crash, but was
**not** adopted — see Gap 2 below for why it had to be reverted.
**Fix**: named the real implementation `sf_sqrt` instead, and `math.h`
exposes the public name via `#define sqrtf(x) sf_sqrt(x)` — the
literal name `sqrtf` never reaches the compiled IR at all, sidestepping
the recognition entirely. Every future `math.h` function that happens
to share a name with a real libm function (`sinf`, `cosf`, `expf`,
`logf`, `powf`, ...) will likely need the same treatment; check for
this specific crash signature early when adding one.

**Gap 2: some float comparisons used as a branch condition crash ISel
— but not in any pattern fully understood.** `sf_sqrt`'s own early-
return guard, `if (a > 0.0f) { ...loop...; return x; } return 0.0f;`,
hit a genuine "Cannot select" crash on the `brcond` it produced —
`llc`'s own DAG dump shows why: it lowers into a *NaN-aware* Select
tree (comparison libcall combined with an `unordsf2` check) rather
than a plain compare-then-branch, and this backend has no ISel pattern
for the resulting shape.

The confusing part: `floorf`/`ceilf`, added in the very same batch,
branch on `f < t`/`f > t` — a *plain* float comparison, no `<=`/`>=`/
`==`/`!=` involved — and compile fine. A methodical attempt to isolate
what actually distinguishes the two (operator direction, comparing
against a literal `0.0f` vs. two named variables, a loop inside the
branch vs. not, returning independent literals vs. values derived from
a shared variable, single-file vs. cross-file compilation, inlining)
found **no single explanatory factor** — every deliberately-simplified
repro that changed only one of these dimensions from `floorf`'s own
exact shape still crashed, except when the function was renamed to
*some* (not all — `truncf` did, `cbrtf` didn't) other real libm names,
and `__attribute__((const))` made no difference either. Not resolved;
genuinely inconsistent by every factor tested so far.

**Fix (worked around, not root-caused)**: rewrote `sf_sqrt`'s guard to
avoid a float comparison operator as a branch condition entirely,
using the same `u32_and_nz`-on-the-sign-bit idiom `sf_add_extract`
already uses safely throughout the soft-float section — check
`sf_bits(a)`'s sign bit and zero-ness directly instead of writing
`a > 0.0f`. This sidesteps the question rather than answering it.
**Anyone adding a new math.h function that needs to branch on a float
comparison should default to this same bit-pattern-guard style**
unless the comparison already matches `floorf`/`ceilf`'s exact proven
shape (a strict `<`/`>` between two named float locals, not a literal).

**Verified**: full existing regression pass unchanged. All four new
functions checked against hand-computed expected output —
`fabsf(-3.5)=3.5`, `floorf(3.7)=3`/`floorf(-3.7)=-4`, `ceilf(3.2)=4`/
`ceilf(-3.2)=-3`, `sqrtf(4)=2`/`sqrtf(2)≈1.414213`/`sqrtf(100)=10`/
`sqrtf(0.25)=0.5` — all exact. `sqrtf`'s 32-iteration Newton's-method
loop, run four times across one program, needed roughly 10x more
`eclipseemu` simulated steps to finish than any test in this project
so far (20,000,000 rather than 2,000,000) — not a bug, just genuinely
that much soft-float computation to step through; worth knowing before
assuming a hang.

### 14. Constant-condition branches ("Cannot select: ... brcond ...") crashed llc (fixed)

Found running the public
[c-testsuite](https://github.com/c-testsuite/c-testsuite) against this
compiler for the first time — 7 of the suite's 220 tests hit the exact
same crash signature, `LLVM ERROR: Cannot select: ... brcond ...`, on
code shapes as simple as `while (1) { ... break; ... }`. This is
almost certainly the same underlying mechanism (not confirmed the
same fix would have helped, but the crash text matches exactly) as an
earlier, unresolved mystery in this file's entry #13 — a `sqrtf`
comparison-as-branch-condition crash that got worked around rather
than root-caused. This entry's fix predates and did not require
revisiting that workaround.

**Root cause**: this target routes *every* compile-time constant,
even small ones like `0` or `1`, through a constant-pool `WRAPPER`
node rather than a plain `ConstantSDNode` — there's no immediate-
operand encoding for arbitrary 16-bit values on this ISA, so even a
literal `1` needs a real memory word to hold it, loaded via `LDA`.
DAGCombiner has a standard, target-independent fold that recognizes a
`brcond` whose condition is provably a compile-time constant (from a
literal `while (1)`, or any comparison the optimizer can resolve at
compile time) and simplifies it — but that generic fold only
recognizes a *plain* `ConstantSDNode` as "a constant," not this
target's `WRAPPER`'d constant-pool form. So the fold only gets half
done: the condition genuinely does collapse down to a bare constant,
but the surrounding `brcond` node itself is never further simplified
into an unconditional branch (or removed entirely) the way it would be
on almost any other target. Worse, this fold runs as part of a later
DAGCombine pass, *after* the legalization pass where this target's own
`Custom` `ISD::BRCOND` lowering (`LowerBRCOND`, which reduces to the
already-correct `LowerBR_CC` path) normally runs — so the newly-
created constant-condition `brcond` never gets a chance to go through
that lowering either. It reaches instruction selection as a raw,
un-lowered `ISD::BRCOND` with a `WRAPPER(ConstantPool<N>)` condition, a
shape this backend has no ISel pattern for at all.

**Fix** (`EclipseISelLowering.cpp`): registered `ISD::BRCOND` for
`setTargetDAGCombine` and added `combineConstantBrcond`, which
recognizes exactly this shape (`brcond`'s condition is a `WRAPPER`
wrapping a `TargetConstantPool` entry holding a `ConstantInt`) and
rewrites it directly — a nonzero constant becomes a plain
unconditional `ISD::BR` to the same destination; a zero constant
becomes just the chain (the branch never fires, so drop it, same as
how a dead conditional branch would ordinarily be removed).

**Verified**: 4 of the 7 originally-crashing tests now pass outright.
The other 3 now hit *different*, pre-existing, unrelated limitations
once past this crash — confirming the fix itself is complete and
correct, these tests just need more than this one fix:
- One needs a variable-length array (`dynamic_stackalloc` — this
  backend has no support for non-compile-time-constant stack
  allocation at all; a real gap, not attempted here).
- One is a single function complex enough to exceed the existing,
  already-documented ±127-word frame-relative displacement limit (the
  same class of constraint `sf_add`/`print_float` needed manual
  splitting to work around — see this file's own comments on that).
- One needs genuine 64-bit `long long` arithmetic, which this target
  does not have (only up to 32-bit `long`) — comparing a `long long`
  against `INT32_MIN`/`INT32_MAX` boundary literals evaluated
  incorrectly, consistent with the 64-bit value silently losing
  precision somewhere rather than a real i64 comparison ever
  happening. Not investigated further; implementing real 64-bit
  arithmetic support is a project on the scale of this session's
  earlier hardware-calling-convention or `math.h` work, not a quick
  fix, and is being tracked as a known gap rather than attempted here.

Full existing regression pass (every package example) and the
existing `math.h`/libc battery all still pass unchanged after this
fix — it only changes behavior for the specific constant-condition-
branch DAG shape described above.

### 15. c-testsuite triage batch: calloc/NULL/sprintf added (fixed), two silent-crash libcall gaps found and fixed, one jump-table ISel gap found and fixed, two new address-computation/register-allocation defects found (not fixed)

Found while triaging the public c-testsuite (`~/dev/c-testsuite`) against
this backend for the first time — 180/220 passing going in. This entry
covers everything from that pass that was either fixed outright or was
significant enough to write up; purely-inapplicable tests (no
filesystem, no `double`, 16-bit `int` where the suite assumes >=32-bit,
etc.) were skip-listed instead — see
`c-testsuite/runners/single-exec/eclipse.skip`.

**Fixed, small, straightforward (rt/eclipse_rt.c and headers):**

- `calloc(nmemb, size)`: didn't exist at all. Added as `malloc` +
  `memset` (reusing the existing, already-correct `memset` rather than a
  new hand-rolled zero loop). `nmemb`/`size` are kept as a plain 16-bit
  `unsigned int` multiply (matching the standard signature) — *not*
  widened to `long` anywhere, per the existing "1000L * 17" 32-bit-
  multiply warning above.
- `NULL`: wasn't defined anywhere (no `stddef.h` on this target).
  `#define NULL ((void *)0)`, guarded with `#ifndef NULL`, added
  redundantly to `stdio.h`/`stdlib.h`/`string.h` — matching how a real
  libc's `stddef.h` ends up pulled in transitively by all three.
- `sprintf(buf, fmt, ...)`: didn't exist. printf's own helpers
  (`print_int`/`print_uint`/...) all call `putchar()` directly with no
  output-sink indirection, so rather than retrofit all of them,
  `sprintf` is an independent, small formatter with its own buffer-
  writing helpers. Supports `%d` (plus one thing printf itself doesn't
  have: an optional `0`-flag + decimal width, e.g. `%02d`, for zero-
  padding), `%c`, `%s`, `%%` — no `%o`/`%x`/`%u`/`%l*` yet.

**Fixed, backend, `EclipseISelLowering.cpp`:**

- **Jump-table ISel crash** (`LLVM ERROR: Cannot select: t4: ch = br_jt
  ...`): a dense/large `switch` (the c-testsuite test is a textbook
  Duff's device) can make `SelectionDAGBuilder` lower it as a jump table
  (`ISD::BR_JT`) instead of a compare chain — this backend has no ISel
  pattern for that opcode at all (only ordinary conditional branches).
  Fixed the same way AVR/MSP430 (other small, in-tree 16-bit-ish
  backends) do it: `setOperationAction(ISD::BR_JT, MVT::Other, Expand)`
  plus `setMinimumJumpTableEntries(UINT_MAX)` in the constructor — the
  latter is what actually stops `SelectionDAGBuilder`'s
  `areJTsAllowed()`/density heuristic from choosing a jump table in the
  first place. Low-risk, proven pattern copied from two other in-tree
  targets, not a novel mechanism. The one c-testsuite test that
  triggered this (00143) still fails to *assemble* after this fix, but
  for an unrelated, pre-existing reason: it's a function with two
  39-element local arrays, well into the same dgasm ±127-word frame-
  displacement limit already documented elsewhere in this file
  (`sf_add`/`print_float`) — see `eclipse.skip`.

- **Silent SIGSEGV (no LLVM ERROR, no assert) inside `LowerCall`,
  reached via `SelectionDAG::getMemcpy`**: an initialized local
  aggregate (e.g. `int Array[10] = {1,2,...};`) can get lowered by
  `SelectionDAGBuilder` as an `llvm.memcpy` intrinsic from a synthesized
  constant global into the stack slot, instead of a store per element,
  once it's past the generic legalizer's inline-expansion threshold.
  `SelectionDAG::getMemcpy()` then falls back to an actual call to the
  `RTLIB::MEMCPY` libcall — which, like every `RTLIB::Libcall` on this
  target (see bug #13's setup comment and the `setLibcallImpl` block in
  `EclipseISelLowering.cpp`'s constructor), defaults to
  `RTLIB::Unsupported` until explicitly wired up. Unlike the float
  libcalls, which fail loudly (`"unsupported library call operation"`),
  this one failed silently: the resulting `ExternalSymbolSDNode` carries
  an *empty* symbol name rather than a null one, so `LowerCall`'s
  `dyn_cast<ExternalSymbolSDNode>` succeeds and the existing
  `report_fatal_error("...indirect...not supported")` guard never fires
  — it SIGSEGVs much later once codegen actually tries to use that empty
  name. Root-caused by temporarily instrumenting `LowerCall` with
  `errs()` prints of the callee's opcode and symbol name right at entry
  — confirmed the crash happens strictly *after* an empty symbol name is
  printed, not before. **Fix**: `eclipse_rt.c` already implements
  `memcpy`/`memmove`/`memset`/`memcmp` directly (`string.h`), so wired
  all four up via `setLibcallImpl(RTLIB::MEMCPY, RTLIB::impl_memcpy)`
  (and MEMMOVE/MEMSET/MEMCMP the same way) in *both* places the float
  libcalls are wired (`EclipseISelLowering.cpp`'s constructor and
  `EclipseSubtarget::initLibcallLoweringInfo` — both are needed, per
  that method's own existing comment, since each reaches a different
  `LibcallLoweringInfo` copy).

  This surfaced a second, independent bug once the four were wired up:
  `eclipse-cc`'s own retry loop (the one that iteratively `-internalize`-
  protects soft-float runtime symbols dgasm reports as
  `"Undefined symbol: __foo"`, since `llc`'s codegen inserts those calls
  *after* the `opt -internalize,globaldce` pass already ran and stripped
  anything it can't see an explicit IR call to) only ever matched names
  starting with `__` — every soft-float libcall happens to be one, so
  this was never noticed. `memcpy` doesn't start with `__`, so
  `"Undefined symbol: memcpy"` fell straight through as a hard failure
  with zero retries. Fixed by broadening the regex from `Undefined
  symbol: __[A-Za-z0-9_]+` to `Undefined symbol: [A-Za-z_][A-Za-z0-9_]*`
  in `eclipse-cc` — covers both symbol families with the exact same
  protect-and-retry mechanism.

**Found, NOT fixed — a real, previously-undiscovered address-computation
defect, live in the same `PerformDAGCombine` HALVE machinery bugs
#4/#5/#9/#12 all already live in:**

Two related but distinct symptoms, both confirmed via minimal repros and
direct assembly tracing (not guessed):

1. **`&local_char_array[N]` for a compile-time-constant odd `N`
   resolves to the same address as `N-1` (or generally `2*(N/2)`)**,
   rather than `N`. Repro: `char a[10]; strcpy(a, "abcdef");
   printf("%s", &a[1]);` prints `"abcdef"` (i.e. `&a[0]`) instead of
   `"bcdef"`. Traced in the generated assembly: computing `&a[1]`
   produces `ADD(FrameIndex, 1)`, which reaches `PerformDAGCombine`'s
   `BaseIsFrameIndex` branch and gets unconditionally halved (`1 / 2 =
   0`, integer division) before being used as the address — exactly the
   same halving this combine deliberately applies to a runtime GEP
   offset into a *numeric* array (where it's correct: 2 bytes pack
   exactly 1 word per element, no information lost). For a local `char`
   array, `EclipseFrameLowering.cpp` (`(MFI.getObjectSize(i) + 1) / 2`)
   *does* reserve only `ceil(size/2)` words of frame space, matching
   that same "2 bytes per word" assumption — but nothing in the actual
   load/store path (`LDFI`/`STFI`/`LDIND`/`STIND`, all confirmed to
   compile straight to plain word-wide `LDA`/`STA` — see
   `EclipseISelLowering.cpp`'s `emitIndirectMem` and
   `EclipseInstrInfo.td`'s `LDABSI`/`STABSI` definitions) ever does the
   corresponding byte-select (shift/mask, or a real read-modify-write
   for stores) to actually split that packed word back into its two
   bytes. So halving computes the right *word* but loses which half of
   it is meant, and a plain word store/load through that address
   clobbers/misreads the other byte outright. (`memset(&a[1], 'r', 4)`
   — c-testsuite 00179 — hits the identical thing, just via `memset`'s
   small-constant-size inline expansion instead of a bare `&a[N]`
   expression; 00180 is the bare-expression repro above almost
   verbatim.)

2. **A struct field reached through a pointer *variable* holding a
   global's address computes a *different* physical word for a STORE
   than for the corresponding LOAD of the exact same field**, when the
   field is neither the struct's first nor last. Repro: `struct ziggy {
   int a, b, c; } bolshevic; ... struct ziggy *tsar = &bolshevic;
   tsar->a = 12; tsar->b = 34; tsar->c = 56; printf("%d %d %d\n",
   tsar->a, tsar->b, tsar->c);` prints `12 0 56` — `.a` and `.c` (the
   first and last fields) round-trip correctly, `.b` (the middle field)
   reads back 0. Traced in the generated assembly instruction-by-
   instruction: the *write* `tsar->b = 34` computes its target address
   as a **plain, unhalved** `ADD` of the byte offset (2) onto the base
   pointer (landing on word offset 2 — actually `bolshevic`'s *third*
   field's slot, since this target's struct globals are laid out as
   separate whole-word symbols with no packing at all — see bug #10).
   The *read* of the same `tsar->b` expression, a few instructions
   later in the same function, computes its address through the full
   `SUB 0,0; DIV`-based HALVE sequence (byte offset 2 → word offset 1),
   correctly landing on the field's real slot — which was never written,
   hence reads back its zero-initialized value. Same DAG shape
   (`ADD(loaded-global-derived-pointer, constant-byte-offset)`) reached
   twice in one function, halved on one path and not the other — a
   genuine internal inconsistency, not a case of "the wrong constant" or
   "the wrong direction." c-testsuite 00205 (a global array of a larger,
   multi-field struct, indexed with a runtime loop variable) produces
   wildly wrong output consistent with the same defect at a scale too
   large to hand-verify field-by-field; not independently traced, just
   attributed to the same mechanism.

**A third, distinct, and more precisely root-caused defect, also NOT
fixed — `MULrr`'s post-RA expansion silently destroys a source operand
that's still live afterward, when the register allocator (reasonably)
believed only its `$dst` was clobbered:**

Repro: `int Array[10]; for (Count=1; Count<=10; Count++) Array[Count-1]
= Count * Count;` then read the array back — prints `1 0 0 4 0 0 0 0 9
0` instead of `1 4 9 16 25 36 49 64 81 100` (c-testsuite 00157). The
*values* that did land somewhere (1, 4, 9) ended up at indices 0, 3, 8
— i.e. exactly `value - 1` (`Count*Count - 1`), not `Count - 1` as
intended. Isolated with a minimal repro that removes the multiply
entirely (`Array[Count-1] = Count;`, no `* Count`) — that version is
byte-for-byte correct, 1 through 10 in order, immediately implicating
`MULrr` specifically rather than the array-indexing machinery bugs
#4/#5/#9/#12 already document.

**Root cause**, read directly from `EclipseInstrInfo.cpp`'s
`expandPostRAPseudo` (the `MULrr`/`UDIVrr`/`UREMrr` post-register-
allocation expansion) and its own `EclipseInstrInfo.td` comment: this
target has exactly two allocatable GPRs (`AC0`, `AC1`) — `AC2` is the
reserved frame pointer. `MULrr`'s *declared* interface to the register
allocator is a plain `$dst = mul $lhs, $rhs` (plus an explicit `Defs =
[AC2]`, save/restored internally) — deliberately **not** also declaring
`AC0`/`AC1` as implicit defs, because (per that `.td` comment) an
earlier attempt at exactly that "made the allocator double-count
register demand for this instruction and made it impossible to
allocate at all" — a previously-hit, previously-fixed problem in this
exact spot. But the real expansion's hardware sequence
(`MOVrr AC2,RHS; MOVrr AC1,LHS; SUBrr AC0,AC0,AC0; MUL; MOVrr Dst,AC1`)
unconditionally overwrites **both** `AC0` and `AC1` — the
`SUBrr AC0,AC0,AC0` zeroing step and the real `MUL` hardware
instruction both touch `AC0` regardless of which physical register
ended up being `$dst`. For `Array[Count-1] = Count * Count`, `Count`
(self-multiplied, so `$lhs == $rhs`) is register-allocated into `AC0`,
with `$dst` assigned `AC1` — a completely reasonable choice from the
allocator's point of view, since only `$dst` (`AC1`) is declared
written, so `AC0` (`Count`) *should* survive. It doesn't: the `SUBrr
AC0,AC0,AC0`/`MUL` steps zero it and then overwrite it with the
product's high word, and the very next use of `Count` (computing
`Count - 1` for the store's index) reads that clobbered `AC0` instead
— which is exactly consistent with the observed values landing at
`Count*Count - 1` instead of `Count - 1`.

This is the same fundamental tension already hit and partially solved
twice in this exact code path (per that `.td` comment): the pseudo
can't correctly declare everything it clobbers (breaks allocation
entirely, verified before) but also can't get away with declaring too
little (silently corrupts a live-through value, verified again now).
A real fix likely needs something structurally different — e.g. forcing
any value live across a `MULrr`/`UDIVrr`/`UREMrr` to be spilled to a
frame slot rather than kept in either GPR, or modeling the true clobber
set some other way the allocator can act on without over-counting
register pressure — not attempted here given how much dedicated
back-and-forth this same instruction's clobber modeling has already
needed (two rounds, per its own comment, before this one). c-testsuite
00157 is skip-listed pending that work.

Neither of the first two was fixed this pass. Both live in exactly the DAG
combine that bugs #4, #5, #9, and #12 above already needed multiple
rounds each to get right, and previous entries in this file are explicit
about how deep that rabbit hole has gone before (see #12's "still-open
gap" and #13's Gap 2, which was never root-caused either). A confident,
low-risk fix wasn't found in the time available — most likely candidates
are (a) making the `BaseIsFrameIndex` halving branch gate on whether the
consuming access is genuinely byte-granular the way the global-string
exception already does for `WRAPPER`'d bases, or (b) making the two
loaded-pointer-derived-from-global write/read paths share one combine
rule instead of apparently diverging partway through legalization — but
either needs the same kind of exhaustive, DAG-dump-driven verification
this file's other HALVE-related entries required, which wasn't
attempted here. c-testsuite tests 00163, 00179, 00180, and 00205 are
skip-listed (not force-"fixed") pending that work.

**Verified**: `bash regress_hwstack.sh`-equivalent full existing
regression pass (every package example, both struct-global tests,
`clobber.c`) still passes unchanged after every fix in this entry.
c-testsuite: 00040 (calloc — also needs recursion and array indexing,
a good integration check; confirmed correct given enough `eclipseemu`
simulated steps, see below), 00171/00179-NULL-only, 00186 (sprintf),
00143 (jump-table ISel crash only — the test still fails to assemble
for the separate frame-overflow reason above), 00185/00208 (memcpy
libcall crash) all individually re-verified against their `.expected`
output. `00040` specifically: correct output confirmed with a 2-billion-
step `eclipseemu` budget (~30s wall time) — it's a genuine full 8-queens
backtracking search, not a hang, but the c-testsuite harness's fixed
5,000,000-step budget isn't enough for it, so it still shows as a
harness-level TIMEOUT rather than PASS under the standard runner.
`00041` (a prime-counting sieve up to 5000) is the same story —
confirmed correct (`AC0 == 0` at `HALT`, matching its own internal
`c == 669` self-check) with a 500-million-step budget, just genuinely
too much simulated computation for the standard 5,000,000-step runner.

Also found, skip-listed, not fixed: `00182` crashes outright ("LLVM
ERROR: unsupported library call operation") on `x % 10L` / `x / 10L`
against an `unsigned long` — this target has no 32-bit integer divide/
remainder libcall implemented or wired at all (only the float ops and,
as of this entry, `memcpy`/`memmove`/`memset`/`memcmp` are). Consistent
with the already-documented "32-bit multiply is only partially
reliable" limitation (bug #12's "1000L * 17") rather than a new,
independent gap.

### 16. Program origin moved from org 0100 to org 050 — reclaims 24 words of page-zero budget (changed convention, documented here)

Not a bug fix — a deliberate change to where every program built by
this toolchain starts, made after noticing several c-testsuite
failures were programs landing just a handful of words over the
shared 256-word page-zero budget (see entries #13's `atof`+`printf`
note and #15's frame-overflow skip-list entries for examples of that
class of failure).

**The observation**: `EclipseAsmPrinter.cpp` emitted a fixed `org
0100` (64 decimal) at the start of every generated program, and
`_start` — along with its hardware-stack-register init code — sits
immediately after that `org` line as part of the fixed preamble
`reorder_asm.py` never reorders (see that file's own header comment),
so `_start`'s address always exactly equals whatever the `org` value
is. `0100` was never derived from anything specific to this backend;
it's simply `dgasm`'s own built-in default address when a source file
has no explicit `org` at all (see `reorder_asm.py`'s `compute_addresses`,
which deliberately keeps its *own* `0o100` default to correctly mirror
that dgasm behavior for the general case — that one is unrelated to
this change and was not touched).

**Checked against the authoritative source** before touching anything,
given how easy it would be to accidentally clobber something hardware-
critical: `eclipseemu`'s own `simh` source, `eclipse_cpu.c`'s header
comment, documents a real, specific purpose for *every* word of memory
from address 0 through 047 octal (0–39 decimal) — not just the ones
this backend itself already uses (0/1 for the interrupt return-
address/vector, 040–042 for the hardware SAVE/RTN stack pointer/frame
pointer/stack limit — see entry on the SAVE/RTN calling convention).
The full table: 0 (I/O return address), 1 (interrupt handler address),
2 (system-call handler), 3 (protection-fault handler), 4/6/7 (VECTOR-
instruction stack pointer/limit/fault address), 5 (interrupt priority
mask), 10 (block pointer, later models), 11 (emulation trap handler,
microEclipse only), 20–27 and 30–37 (auto-increment/auto-decrement
*indirect-addressing* locations — genuinely dangerous to repurpose for
ordinary data, since indirectly referencing one of these auto-
increments or -decrements it as a hardware side effect regardless of
what the program intended it to hold), 40–43 (the stack registers this
backend already uses), 44 (XOP origin), 45 (floating-point fault), 46
(commercial-instruction-set fault), and 47 itself documented in so
many words as "Reserved, do not use." `050` is the first word after
all of that.

**Change**: `EclipseAsmPrinter.cpp` now emits `org 050` instead of `org
0100`. This moves every program's `_start` (and therefore every
subsequent address) down by 24 decimal words, which is also 24 more
words of page-zero budget reclaimed for constant-pool/call-slot/
address-slot data, at zero risk beyond what's already covered by the
table above.

**This changes the toolchain's entry-point convention project-wide**:
every existing `dep PC 100` in this README, in `DEBUGGING_NOTES.md`'s
own reproduction commands, and in any external scripts/notes/muscle
memory needs to become `dep PC 50`. Updated everywhere in this
package; if you have your own scripts or notes referencing `dep PC
100` from before this change, they need the same update. This applies
identically to the sibling `nova-llvm-backend` package, which shares
this exact file (`EclipseAsmPrinter.cpp`) and picked up the same
change in its own sync from this session's work.

**Verified**: full existing regression pass (every package example)
matches established baselines exactly under the new entry point,
including `isr_c_test`'s and the `fps_*` tests' already-documented
non-fatal outcomes. All previously-passing c-testsuite results
unchanged; a program that previously failed to compile exactly 1 word
over the page-zero budget (an `atof()` + `printf()` combination — see
entry #13's own note on this) now compiles and runs correctly.

### 17. Indirect (function-pointer) call support added (fixed) — plus an unrelated printf `%i` gap and a global function-pointer-initializer gap found and fixed alongside it

Not a bug fix on its own to start — this backend previously had no support
at all for calling through a function-pointer *value* computed at runtime
(as opposed to a direct call to a compile-time-known symbol, which every
call before this went through). `EclipseISelLowering.cpp`'s `LowerCall`
`report_fatal_error`'d unconditionally on any callee that wasn't a
`GlobalAddressSDNode`/`ExternalSymbolSDNode`. Four c-testsuite tests
(00087, 00089, 00124, 00210) depended on this, plus 00216 (skip-listed
specifically for hitting this same error).

**ISA research first, before writing any code**: every existing call
(direct or not) already goes through one level of indirection — `JSR
@<callee>_SLOT,0`, where `<callee>_SLOT` is a *compile-time* page-zero
`var` line holding the real target address, because `JSR`'s addressing
modes need a page-zero-resident operand the same way `LDA`/`STA` do (see
`CALL`'s own comment in `EclipseInstrInfo.td`). The question this entry
had to answer empirically: does real Nova/Eclipse `JSR` have any mode that
jumps to an address held in a *register* directly, sidestepping the need
for a page-zero word at all? Checked against the same authoritative source
this project already trusts for ISA facts (the DG Nova/Eclipse instruction
set `JSR` shares its addressing-mode field with `LDA`/`STA`/`JMP`) — the
answer is no: `JSR`'s addressing modes are direct, AC1/AC2/AC3-indexed,
and indirect-through-*memory*, exactly the same set every other
memory-referencing instruction on this ISA has, never "jump to whatever's
in a register." So a function-pointer *value* sitting in AC0/AC1 still
needs to land in an addressable memory word before `JSR` can reach it —
there's no way around at least one extra store.

**The mechanism chosen**: reuse the existing shared page-zero `_scratch`
word — the same fixed word `LDIND`/`STIND`'s `emitIndirectMem` already
uses for general pointer dereference (`STA` the address into `_scratch`,
then `LDA`/`STA ac,@_scratch`). For an indirect call: `STA` the
function-pointer value into `_scratch`, then `JSR @_scratch,0`. Safe for
the same reason `emitIndirectMem`'s reuse of `_scratch` already is: the
store and the following indirect operation are always emitted back-to-back
by the same custom inserter with nothing else scheduled in between, so two
different indirect operations can never see each other's half-written
state. `_scratch` is unconditionally declared (`var _scratch = 0`) in
every generated program regardless of whether anything uses it, so no new
page-zero declaration bookkeeping was needed.

**Implementation** (`EclipseISelLowering.h`/`.cpp`, `EclipseInstrInfo.td`
— `EclipseAsmPrinter.cpp` needed no changes at all, which is itself worth
noting: see below):

- A new target node, `EclipseISD::CALL_INDIRECT`, structurally distinct
  from `EclipseISD::CALL` — same variadic-operand type profile, but the
  one real operand is a plain i16 GPR value instead of a `texternalsym`.
  `LowerCall` now branches on the callee: `GlobalAddressSDNode`/
  `ExternalSymbolSDNode` still build the `<callee>_SLOT` name and emit
  `EclipseISD::CALL` exactly as before (the `else` branch that used to be
  `report_fatal_error` is simply gone — direct calls are completely
  unchanged); anything else emits `EclipseISD::CALL_INDIRECT` with the
  already-materialized-into-a-register callee `SDValue` as its operand
  (it arrives as an ordinary i16 SSA value with no special handling
  needed — a function pointer is "just a value" everywhere else in this
  backend already, including through struct fields, parameters, and
  return values, none of which needed any change).
- `EclipseInstrInfo.td`: `CALLIND`, a `usesCustomInserter` pseudo taking
  one `GPR` operand, matching `(EclipseCallIndirect GPR:$func)` — mirrors
  `LDIND`/`STIND`/`PUSH`/`POP`'s existing pattern exactly. It expands (in
  `EclipseISelLowering.cpp`'s new `emitCallIndirect`, dispatched from
  `EmitInstrWithCustomInserter` next to the other custom-inserted
  pseudos) into two real `MachineInstr`s: `STABS $func, "_scratch"`
  (the exact same instruction `emitIndirectMem` already builds this way —
  no new instruction needed for the store half) followed by a new fixed,
  operand-less real instruction, `CALLIND_JSR`, whose `AsmString` is
  literally `"JSR @_scratch,0"`. `CALLIND_JSR` carries the identical
  `isCall = 1, Defs = [AC0, AC1, AC3], Uses = [AC2]` flags `CALL` itself
  has, deliberately on the *real* post-expansion instruction rather than
  the pseudo (which is erased before register allocation ever runs, same
  as every other custom-inserted pseudo here) — this is what tells the
  register allocator an indirect call clobbers exactly the same registers
  a direct one does.
- Why `EclipseAsmPrinter.cpp` needed no changes: `case Eclipse::CALL:` in
  `emitInstruction` special-cases `CALL` specifically to read its symbol
  operand and register a `<callee>_SLOT` `var` line for it. `CALLIND_JSR`
  deliberately avoids that path entirely by being a distinct opcode with a
  fixed, operand-less `AsmString` — it needs no symbol lookup, no `_SLOT`
  registration, nothing beyond what the default `EclipseMCInstLower`-based
  instruction printer already does for every ordinary instruction. An
  earlier design considered reusing `CALL` itself with `_scratch` as its
  symbol operand (since `CALL`'s `calltarget` operand already accepts any
  external symbol) — rejected once traced through: it would have
  registered `_scratch` as a `CallSlots` entry too, emitting a bogus
  second `var _scratch = _scratch` line alongside the real `var _scratch =
  0` line already emitted unconditionally at the top of every file,
  since `stripSlotSuffix` only strips a literal `_SLOT` suffix and leaves
  `_scratch` unchanged.

**Verified empirically at every step, not just assumed** (per this
project's own established methodology): confirmed via `clang -cc1 -S` on
c-testsuite 00087 (`struct S { int (*fptr)(); }; v.fptr = foo; return
v.fptr();`) that the generated assembly is exactly `LDA 0,foo_PTR` /
`STA 0,6,2` (store into the struct field) / `LDA 0,6,2` (reload) /
`STA 0,_scratch` / `JSR @_scratch,0` — no `_SLOT` reference anywhere for
`foo`, confirming the whole point of this feature (no compile-time-known
callee symbol needed). Ran on `eclipseemu` (`dep PC 50`, per entry #16):
00087, 00089, 00124, and 00210 all PASS against their `.expected` output.

**00089 needed a second, unrelated, pre-existing fix to actually pass**:
initially hung (`Step expired, PC: 00000 (JMP 0)` — the same "corrupted
SAVE/RTN linkage" trap signature documented in entry #11) even with the
indirect-call mechanism itself working correctly (confirmed via a battery
of isolated repros: two sequential indirect calls in one function, a
function returning a function pointer that's immediately called
(`go()()`), and calling through a struct pointer's function-pointer field
— the first two passed cleanly in isolation). The minimal repro that
finally reproduced it was a **global** (not local) struct initialized with
a function pointer: `struct S { int (*zerofunc)(); } s = { &zero };`.
Root-caused by inspecting the generated assembly directly: `var s = 0`
instead of `var s = zero` — `EclipseAsmPrinter.cpp`'s `flattenConstant`
(added in entry #10 for struct-global support) has a documented, explicit
fallback for "a field type genuinely unsupported here (a pointer, a
float)" that degrades to a literal zero word. That fallback was correct
for a float (this target has no way to materialize one at compile time)
but not for a pointer to another global — `dgasm` already resolves a bare
symbol reference in a `var NAME = OTHERSYMBOL` line to `OTHERSYMBOL`'s own
assigned address, exactly the mechanism this backend already relies on
for every `_SLOT`/`_PTR` slot it emits elsewhere. **Fix**: `flattenConstant`
now recognizes a `GlobalValue` (a `Function*` or `GlobalVariable*` used
directly as a constant — what `&function`/a data global's address actually
look like at the IR level) as a leaf and emits `getSymbol(GV)->getName()`
instead of `"0"`; changed `flattenConstant`'s output type from
`SmallVector<int64_t>` to `SmallVector<std::string>` to carry a symbol
reference alongside ordinary numeric words. The same fix was added as a
new top-level case in `emitGlobalVariable` itself, ahead of the
`ConstantInt` scalar case, for the simpler (non-struct) shape — a
top-level global directly initialized with another global's address (e.g.
`int (*fp)() = foo;`), not exercised by any of the four target tests but
the same gap by the same root cause, cheap to close alongside it.
Verified: 00089 now passes; re-checked entry #10's own two struct-global
regression tests (`struct_global`/`struct_zero` in
`regress_hwstack.sh`) still produce byte-identical output.

**00210 needed a third, also unrelated, pre-existing fix**: compiled and
ran to a clean `HALT`, but produced no visible digits for either of its
two `printf("%i\n", ...)` calls — traced to `eclipse_rt.c`'s `printf`
implementation never having supported the `%i` conversion specifier at
all (only `%d`/`%o`/`%c`/`%s`/`%x`/`%u`/`%l[du]`/`%%`), so the `if`-chain
fell through untaken for `'i'` and no digits were ever emitted (the
format string's own literal `\n` right after still printed normally,
which combined with this project's own test harness stripping exactly one
blank-line/banner pair made the missing output look like "nothing
printed at all" rather than "one bare newline" — confirmed the real
shape via `cat -A` on the raw `eclipseemu` output, finding a literal
`^M$` / `$` pair where the digits should have been). **Fix**: `%i` treated
as a synonym for `%d` (standard C `printf` behavior — they differ only in
`scanf`), a one-line addition to the same `if (*fmt == 'd')` check. Synced
into `eclipse-package/eclipse-toolchain/rt/eclipse_rt.c` alongside the
`llvm-project` changes, per this package's usual convention for runtime
changes.

**00216** (skip-listed specifically for hitting the old indirect-call
error) now compiles past that error, confirming the fix generalizes to
its function-pointer-table shape (`table[i]()`, `p = global_wrap[0].func;
p();`) too — but still doesn't pass, for two separate, unrelated,
already-documented reasons: several frame-relative accesses exceed
dgasm's ±127-word displacement limit (the same class of constraint
`sf_add`/`print_float` needed manual splitting to work around), and the
program's total call/branch-target count overflows the shared 256-word
page-zero budget outright (dgasm reports addresses up to 261, "should be
0 - 255"). Neither is remotely related to indirect calls — this one
program is simply large enough to hit two separate, pre-existing scaling
limits. Left in `eclipse.skip`, comment updated to describe the current
(different) failure reason rather than the old, now-fixed one.

**Full regression verified**: `regress_hwstack.sh` produces output
byte-identical to a freshly-captured pre-change baseline (via `git stash`
on the `llvm-project` changes and a saved copy of the pre-fix
`eclipse_rt.c`, rebuilding `llc` for each side) for every one of its 15
examples — same printed output on every line, same 11 `HALT` / 4
`Step expired` (benign-hang) classification, both before and after. The
only differences in the raw before/after logs at all are the disassembly
text shown for the final halted/trapped instruction's own address on a
handful of examples (a few words of address drift from `printf` gaining
the one-line `%i` check, present in every example that links it in) —
expected and harmless, not a behavior change. Also ran the full
c-testsuite (`bash run_full_suite.sh`, a new small harness script wrapping
`run_one_test.sh` over every non-skip-listed test): 192 PASS, 1 MISMATCH,
2 COMPILE_FAIL, 2 TIMEOUT out of 197 run. The 2 TIMEOUTs (00040, 00041)
are the already-documented "genuinely correct, just too slow for the
harness's fixed step budget" cases from entry #15. The other 3
(00200/00203: `long long` shift-type and comparison tests; 00207: a
variable-length-array test) were independently re-confirmed to fail
*identically* — same COMPILE_FAIL/MISMATCH classification — against the
unmodified pre-change baseline, i.e. pre-existing gaps (no 64-bit integer
support; no VLA support, both already documented elsewhere in this file)
this pass simply never touched, not regressions.

### 18. Constant struct-field offset into a *direct* global getting silently un-folded (fixed) — a real but narrower cousin of entry #15's still-open write/read asymmetry

Found while investigating entry #15's HALVE-machinery bugs further.
Distinct from all four tests still skip-listed for that entry — this
one specifically covers a global struct field written or read
*directly* (`bolshevic.b = 34;`), not through an intermediate pointer
variable (`tsar->b = 34;` where `tsar = &bolshevic;`) — see the
"scope, precisely" note below for why that distinction matters and
why this does *not* close out entry #15's own test cases.

**Root cause**: a compile-time-constant `getelementptr(i8, @global,
N)` (an ordinary struct-field access on a global reached directly) is
supposed to be constant-folded by generic SelectionDAG machinery
straight into `GlobalAddressSDNode`'s own `Offset` field, before this
backend ever sees it as a separate `ADD`. Confirmed via `llc
-debug-only=isel` that this fold was silently never happening on this
target at all: `TargetLowering::isOffsetFoldingLegal`'s default
implementation falls through to `false` here, because this target's
triple (`eclipse-dg-none`) has none of the object formats
(ELF/COFF/MachO/Wasm/XCOFF/GOFF) that default check looks for, and
`clang -cc1` never emits `dso_local` on this triple either.

The practical effect: a constant struct-field GEP into a global
reached `PerformDAGCombine` as a genuine, un-folded `ADD(GlobalAddress,
Constant)` instead of a pre-folded `GlobalAddress`-with-offset.
`PerformDAGCombine`'s own "constant offset into a global" guard
*deliberately* leaves such an `ADD` un-halved (that guard exists for a
different, already-fixed bug — the type legalizer's own
already-word-granular "access word N of an oversized global" address
computation, which must not be halved again — see entry #12's earlier
work in this exact function) — so the struct-field byte offset fell
through ISel as a plain, unhalved runtime add: off by a factor of 2,
landing on the wrong field's word.

**Fix**: override `isOffsetFoldingLegal` in `EclipseISelLowering.h` to
unconditionally return `true`, rather than touch
`shouldAssumeDSOLocal` or the triple's object-format classification
(both much bigger-blast-radius, target-independent changes affecting
things well outside this backend). This target's relocation model is
already forced to `Reloc::Static` unconditionally
(`EclipseTargetMachine.cpp`), and on this freestanding, linker-less,
single-binary target every global's address really is known and fixed
at assemble time — the whole DSO-locality question this hook exists to
answer doesn't apply here, so reporting every global as foldable is
correct, not merely expedient.

**Scope, precisely — does NOT fix entry #15's open tests**: verified
directly that `00163`, `00179`, `00180`, and `00205` (the tests
skip-listed for entry #15) still fail identically with this fix in
place. Those all reach the struct field through a *loaded pointer
variable*, a structurally different DAG shape (the `ADD`'s base
operand is a value reloaded from its own stack slot, not a direct
`GlobalAddressSDNode`) that `isOffsetFoldingLegal` has no bearing on —
entry #15's own writeup already anticipated these might be separate
mechanisms needing separate fixes ("make the two loaded-pointer-
derived-from-global write/read paths share one combine rule instead of
apparently diverging partway through legalization"). This entry closes
out the *direct*-global-access half of that picture; the
loaded-pointer half remains open, still skip-listed, still not
force-fixed given the same fragile history entry #15 documents.

**Verified**: full existing regression pass unchanged (byte-identical
output on every package example). A new direct-global-struct-field
repro (`bolshevic.a/.b/.c = 12/34/56;` with no pointer variable
involved) reads back correctly (`12 34 56`) with this fix in place.

### 19. Variable-length array support added (DYNAMIC_STACKALLOC / STACKSAVE / STACKRESTORE)

C variable-length arrays (`int n = ...; int buf[n];`) previously hit
"Cannot select" crashes in llc -- `ISD::DYNAMIC_STACKALLOC` had no
lowering at all on this target.

**Design**: `DYNAMIC_STACKALLOC` lowers to a new `VLAALLOC` pseudo,
custom-inserted as three real instructions: `LDABS` the current
hardware stack pointer (mem[040], the same SAVE/RTN stack-pointer word
entry #11 introduced) as the base, `ADDrr` it with the requested word
count, `STABS` the sum back as the new stack pointer. A single `ADD` is
used rather than an unrolled loop (the pattern `emitAdjCallStack` uses
for fixed-size stack adjustment) because the word count here is a
genuine runtime value, not a compile-time constant, so there's nothing
to unroll.

Byte count to word count uses the existing `EclipseISD::HALVE` node
((size + 1) / 2, rounding up), not a fresh `ISD::SRL`-by-1 -- see the
first bug below for why.

While bringing up a minimal repro, clang turned out to emit
`llvm.stacksave`/`llvm.stackrestore` unconditionally around *any*
lexical block containing a VLA, even a top-level block with no loop or
early exit -- not something the minimal design above originally
accounted for. Both are handled as trivial single-instruction custom
pseudos: `STACKSAVE_PSEUDO` is a direct `LDABS` of mem[040], and
`STACKRESTORE_PSEUDO` is a direct `STABS` back to mem[040], discarding
everything allocated since (including VLA words), exactly the same
unconditional reset `RTN`'s own epilogue already does at function
return (see `EclipseFrameLowering.cpp`'s `emitEpilogue`).

**Bugs found and fixed along the way, all real, all caught before or
during bring-up rather than left latent**:

1. **A shift built fresh during Custom-lowering tripped `LowerShift`'s
   strict constant check.** The word-count computation originally used
   `ISD::SRL` by a freshly-built `DAG.getConstant(1, ...)`, but
   `LowerShift`'s `dyn_cast<ConstantSDNode>` on the shift amount failed
   even though the operand genuinely was a constant -- an ordering
   quirk specific to nodes built *during* Custom-lowering itself: the
   constant hasn't yet been independently routed through `ISD::Constant`'s
   own Custom-lowering (constant-pool placement) by the time
   `LowerShift` inspects it. Fixed by using `EclipseISD::HALVE` instead,
   which selects straight to `UDIVrr` and bypasses `LowerShift`/
   `ISD::SRL` entirely -- the same node `PerformDAGCombine`'s existing
   halving code already relies on, built the same way (via
   `DAG.getConstantPool` + `LowerConstantPool` for the "1" and "2"
   constants).

2. **`LDABSI` (indirect) used where `LDABS` (direct) was needed.**
   `LDABSI` means "LDA $dst,@$addr" = `mem[mem[$addr]]` -- correct for
   `emitPushPop`'s `POP`, which dereferences through the pointer to
   retrieve a previously-pushed *value*. VLA alloc instead needs the
   raw pointer value stored *at* mem[040] itself: `LDABS`, no
   indirection. Caught via code review (re-reading `emitPushPop`'s
   actual semantics) before it could manifest as corrupted data --
   an earlier, unrelated gap (bug 3 below) crashed first.

3. **`ISD::STACKSAVE`/`ISD::STACKRESTORE` unhandled.** Not scoped for
   originally; discovered via `"Cannot select: t56: ch = stackrestore
   ..."` once VLA alloc itself worked. Fixed as described above.

4. **`DAG.getMachineNode` operand order backwards**, twice
   (`VLAALLOC`'s `{Chain, WordCount}` and `STACKRESTORE_PSEUDO`'s
   `{Chain, SavedPtr}`). The established convention -- documented
   directly on `PUSH`'s own construction in `LowerCall`: "value
   operand(s) first, chain last" -- is the opposite order. Both hit
   `InstrEmitter::EmitMachineNode`'s "#operands for dag node doesn't
   match .td file!" assertion until reordered to match `PUSH`'s
   pattern.

5. **A virtual register reused as its own tied output, violating the
   pre-RA single-definition invariant.** `emitVLAAlloc`'s `ADDrr` step
   initially wrote its result back into the same `WordCount` virtual
   register already used as the pseudo's input. The tied "$dst=$dstin"
   constraint is a hint for the *register allocator* to later coalesce
   physical registers -- it doesn't license reusing a virtual register
   number as a second definition at this stage. Hit
   `MachineRegisterInfo::getVRegDef`'s "at most one definition"
   assertion; fixed by allocating a fresh virtual register
   (`MRI.createVirtualRegister(&Eclipse::GPRRegClass)`) for `ADDrr`'s
   result, matching the only other place in this codebase that calls
   `createVirtualRegister` (`EclipseRegisterInfo.cpp`'s
   `eliminateFrameIndex`).

**Verified**: a minimal repro (`int n = 4; int buf[n];` filled and
read back via `printf`) prints the correct values on `eclipseemu`.
c-testsuite `00207` passes. Full existing regression suite
(`regress_hwstack.sh`) re-run clean, byte-identical to baseline --
no regressions in float, struct, or hardware-stack (SAVE/RTN)
handling from this change.

### 20. 32-bit (`long`) division/remainder had no runtime at all -- `llc` hard-crashed (fixed), plus a second, previously-latent "Select feeding a brcond" ISel crash found and fixed alongside it

`ISD::UDIV`/`SDIV`/`UREM`/`SREM` only had `Custom` lowering for
`MVT::i16` in `EclipseISelLowering.cpp`'s constructor -- the hardware
`DIV` instruction (driven through `UDIVrr`/`UREMrr`, see
`LowerSDIVREM`) is 16-bit only. `MVT::i32` (this target's
`long`/`unsigned long`) has no register class at all, so the type
legalizer's default int-widening path
(`DAGTypeLegalizer::ExpandIntRes_UDIV`/`SDIV`/`UREM`/`SREM`) fell
straight through to `RTLIB::UDIV_I32`/`SDIV_I32`/`UREM_I32`/`SREM_I32`
-- and like every other libcall on this target before it gets wired
up, all four defaulted to `RTLIB::Unsupported`. Any program dividing a
`long` crashed `llc` outright ("LLVM ERROR: unsupported library call
operation"), reproduced with c-testsuite's `00182.c` (`print_led`,
which extracts decimal digits from an `unsigned long` via repeated
`/10` and `%10` in a `while` loop).

**Fix, part 1 (the division libcalls themselves)**: `eclipse_rt.c`
now implements `__udivsi3`/`__umodsi3` directly -- a restoring
shift-subtract long-division bit loop, the same style as the existing
`u32_div10`/`sf_divbits` helpers (every shift is by the compile-time
constant 1, looped at runtime, since variable-*amount* shifts have no
lowering on this backend -- confirmed `ISD::SRL_PARTS`/`SHL_PARTS`
"Cannot select" if attempted directly; see those functions' own
comments), generalized from `u32_div10`'s fixed divisor of 10 to an
arbitrary runtime divisor. `__divsi3`/`__modsi3` are derived from the
unsigned primitive with abs-value + conditional-negate sign handling,
the same convention `LowerSDIVREM` already uses for the native 16-bit
case. The remainder comes back through a file-scope static rather
than an output parameter, for the same reason `u32_div10_rem` does
(see that variable's comment): writing a 32-bit value through a
pointer *parameter* is silently discarded on this backend. Wired up
via `setLibcallImpl` in `EclipseISelLowering.cpp`'s constructor, the
same way the float libcalls and the memcpy family already are (`RTLIB
::UDIV_I32 -> RTLIB::impl___udivsi3`, etc. -- confirmed these exact
`RTLIB::Libcall`/`RTLIB::LibcallImpl` enum names by grepping the
generated `RuntimeLibcalls.inc` and cross-checking
`RuntimeLibcalls.td`, not assumed).

**Fix, part 2 (a second, previously-latent crash uncovered by part 1)**:
wiring up division alone got `00182.c` past its original crash and
straight into a *different* crash in the same function: "Cannot
select: ... brcond ... Select ...". This turned out to be the exact
failure mode entry #14 already diagnosed and partly fixed --
`DAGCombiner`'s final combine pass (the one that runs *after*
`Legalize()`, i.e. after this target's `BRCOND` `Custom` lowering
already had its one chance to run) can rebuild a fresh `ISD::BRCOND`
node that reaches ISel un-lowered. Entry #14's `combineConstantBrcond`
only recognizes one specific shape of that problem (a `WRAPPER`'d
constant-pool condition, from a compile-time-provable `while(1)`-style
branch); this is a second, structurally different shape of the same
underlying gap: `x == 0` (a 32-bit comparison, from `00182.c`'s
`while(x)` digit-extraction loop) lowers through `LowerSELECT_CC` into
an `Eclipse::Select`/`SelectU` *machine* node, and that combine pass
folds the machine node straight into the loop's own `brcond` -- a
shape `combineConstantBrcond`'s `WRAPPER(TargetConstantPool)` check
doesn't match, so it fell through unconverted.

Fixed the same way entry #14 fixed the first shape: recognize it in
the existing `ISD::BRCOND` combine (`PerformDAGCombine`) and reduce it
through the already-robust `LowerBR_CC` path (the same "branch if
Cond != 0" reduction `LowerBRCOND` itself would have done, had it
gotten the chance). The check is `Cond.isMachineOpcode() &&
(Cond.getMachineOpcode() == Eclipse::Select || ... == Eclipse::SelectU)`
-- an unconditional structural guard, not gated on `DAGCombinerInfo`'s
legalization-stage flags, for the same reason `combineConstantBrcond`'s
own `WRAPPER` check isn't either: a `Select`/`SelectU` machine node,
like a `WRAPPER`'d constant, only ever exists *after* `Custom`
lowering has already run once, so it structurally cannot misfire on an
ordinary `brcond` fresh from `SelectionDAGBuilder`.

**Verified**: `00182.c` now compiles, assembles, and runs correctly on
`eclipseemu`, matching its `.expected` output exactly (via the
project's standard compile-run-diff harness). Full existing
`regress_hwstack.sh` regression suite produces byte-identical output
to a baseline build (`EclipseISelLowering.cpp` reverted to its
pre-this-change committed state via `git stash`, rebuilt, re-run) --
including the pre-existing "Step expired" cases (`isr_c_test`,
`test_fps_add`, `test_fps_md`, `fps_dma_test`), confirmed present in
that same baseline run too, i.e. not a new regression from this
change. Full c-testsuite pass count went from 193/220 to 196/220 --
`00182` confirmed as one of the three; the other two newly-passing
tests weren't individually root-caused beyond "some other program
also happened to divide a `long`", since that wasn't required to
confirm this fix is correct and non-regressing. The remaining 24
failures (`COMPILE_FAIL`/`MISMATCH`/`TIMEOUT`) are unrelated,
pre-existing limitations, not investigated further here.

### 21. Large stack frames overflowed indexed addressing's 8-bit displacement (fixed), plus a page-zero-budget and register-scavenger fallout each fix along the way

Found via three failing c-testsuite tests: `00128`, `00143`, `00200` all
failed at the `dgasm` assembly step, not `llc` — `Address out of range.
Got 274, should be -128 - 127` on lines like `STA 0,274,2` / `LDA
1,270,2`. All three have large local-variable frames.

**Root cause**: `EclipseRegisterInfo::eliminateFrameIndex` resolves
`LDFI`/`STFI` (AC2/FP-relative loads and stores) by substituting the
stack object's computed FP-relative displacement straight into the
indexed-addressing instruction's immediate operand, unconditionally —
no check against real hardware's actual encoding limit: indexed
addressing (index=2, i.e. AC2-relative) only has an 8-bit *signed*
displacement field, -128..127. A large enough frame (enough locals, or
big-enough arrays) pushes some variable's offset past that trivially,
and `dgasm` correctly rejects the resulting instruction — this is a
genuine hardware constraint, not an assembler bug. (The sibling `LEAFI`
pseudo, right above this code — `&local`'s address-of — already had to
solve exactly this problem for a different reason: there are no
immediate-operand instructions on this ISA at all, so *any* frame
offset needs materializing through a constant-pool load, not just
out-of-range ones.)

**Fix, part 1 (the addressing fallback)**: when the offset doesn't fit
-128..127, `eliminateFrameIndex` now falls back to the same
address-materialize-then-indirect-through-page-zero mechanism
`LDIND`/`STIND`'s `emitIndirectMem` (`EclipseISelLowering.cpp`) already
uses for a runtime pointer value, and `LEAFI` already uses for
`&local`: build the real address (FP + Offset) into a register, stash
it in the shared page-zero `_scratch` word, and access memory through
`@_scratch` indirection. Works for both `LDFI` (read) and `STFI`
(write).

**Fix, part 2 (page-zero budget)**: the obvious way to materialize
Offset — one `MF.getConstantPool()` entry holding the literal value —
worked for `00128`/`00143` but not `00200`, which has 58 *separately*
out-of-range locals (one big block of small temporaries) with 58
different offsets. Page-zero data, constant pool entries included, is a
hard, shared 256-word budget across the *whole program* (see
`eclipse-cc`'s own comment on this) — 58 one-off constants alone
overflowed it (confirmed: after fixing part 1, `00128`/`00143` passed
outright but `00200` failed with a *new*, different error, `JMP
@LBBx_SLOT,0` indirect jump-table slots landing past address 255 —
i.e., the fix was correct but too page-zero-expensive). Switched to
decomposing `|Offset|` into its power-of-two components and
accumulating them instead of materializing the full value at once:
every out-of-range site in a function now only ever needs a constant
for a bit *value* (1, 2, 4, ... at most 16 of them for an `i16`), and
`MachineConstantPool` already dedups identical constants within a
function — so 58 different offsets sharing the same handful of set
bits collectively costs at most ~16 words instead of 58.

**Fix, part 3 (register-scavenger fallout, twice)**: the
address-decomposition loop needs two scratch registers live at once
(the running total, plus the bit currently being folded in) — and this
target's allocatable `GPR` class has only two members to begin with
(AC0/AC1; AC2/AC3 are reserved as FP/return-address). First fallout: a
store's operand 0 (the value being stored) needs to stay live through
that whole loop too, and protecting it as a *third* concurrently-live
value crashed the register allocator outright — `Error while trying to
spill AC0 from class GPR: Cannot scavenge register without an emergency
spill slot!` — even on `00128`, a comparatively modest frame. Fixed by
stashing the store's value into `_scratch` up front (reloaded by value
right before the final indirect store overwrites `_scratch` with the
address instead), freeing its register for the decomposition loop
instead of pinning it. Second fallout: even with that fix, `00128`
still crashed the same way once the *bit-decomposition* version of the
fix was in place (it hadn't, with the simpler single-constant version
from part 2's "obvious" first attempt) — `-debug-only=reg-scavenging`
showed the backward scavenger successfully spilling/reloading AC0 and
AC1 dozens of times over the course of the function, then finally
failing to spill AC0 specifically: with a large enough frame, some
point deep in a block genuinely needs *both* AC0 and AC1 spilled
simultaneously (nested), which entry #6's original single emergency
spill slot (`EclipseFrameLowering::processFunctionBeforeFrameFinalized`)
can't represent no matter how many times it's reused serially.
`RegScavenger` natively supports more than one scavenging slot (its own
`Scavenged` list); reserving a second one-word slot fixed it.

**Verified**: `00128`/`00143`/`00200` all now assemble and run to the
correct `HALT`/output on `eclipseemu` (previously all three failed to
assemble). Full c-testsuite: 197/220 passing, up from a freshly
re-measured 194/220 baseline (this session's own `git stash` of just
these two files, rebuilt, re-run — not the 193/220 figure quoted
elsewhere in this log, which predates entry #20's division-libcall
fix); diffing the two runs' `PASS` sets directly confirms the
newly-passing set is *exactly* `{00128, 00143, 00200}`, with no
previously-passing test regressing. `regress_hwstack.sh`'s existing
examples produce byte-identical program output before and after this
change (`char_test`, `isr_c_test`, `printf_octal_check`, `sizeof_check`,
the float tests, the FPU tests, `clobber`, `struct_global`,
`struct_zero`) — the only diffs are in the trailing
disassembly-of-incidental-memory shown at each `HALT`/timeout PC, which
shifts along with this fix's code-layout changes and was never
meaningful program state.

### 22. Direct calls via `EJSR`, eliminating the page-zero call-table slot (changed design, verified)

Not a bug fix — a deliberate design change once real Eclipse S/140
*extended addressing* was confirmed to exist and work on this exact
toolchain. Every previous entry in this log (and the backend's own
README) treated "no immediate operands, only page-zero (0-255 word)
absolute addressing, or ±127-word PC/AC-relative addressing" as the
hard ISA ceiling — true for the base Nova instruction set this backend
had exclusively targeted so far, and the entire reason `CALL` went
through a page-zero jump table (`var <callee>_SLOT = <callee>`, `JSR
@<callee>_SLOT,0`) instead of a direct `JSR`: a direct `JSR`'s signed
displacement genuinely can't reach an arbitrarily distant function on
real hardware.

**What's actually there**: real Eclipse (not the base Nova ISA) has a
genuine extended-addressing instruction family — `ELDA`/`ESTA`/`EJMP`/
`EJSR`/`ELEF`/`EISZ`/`EDSZ`/`ELDB`/`ESTB` — each a 2-word encoding whose
second word holds a real address reaching the *entire* 32K-word address
space directly, no page-zero indirection needed. Confirmed against the
real S/140 Programmer's Reference and empirically on this exact
`dgasm`/`eclipseemu` toolchain: a hand-written `EJSR farfunc` correctly
saved its return address in AC3 (matching this backend's existing
calling convention) and jumped/returned correctly across a distance
(050 to 500 octal) far exceeding the base-ISA displacement limit.

**Addressing-mode subtlety, checked before relying on it**: `dgasm`'s
one-operand `EJSR $func` form auto-selects *PC-relative* addressing
(confirmed by reading `opcode.c`'s `encode_extendedflow_instruction`:
`argc == 1` defaults `index` to 1, i.e. relative), which still has a
real ±16383-word range limit `dgasm` enforces at assemble time. The
explicit three-operand form (`EJSR $func,0`) forces *absolute*
addressing instead — unsigned 0-32767, this target's entire address
space, no distance limit at all. This backend now always emits the
explicit `,0` form for exactly this reason: no generated program has
come remotely close to the relative limit, but there's no reason to
depend on that holding rather than removing the question entirely.

**Change**: `EclipseInstrInfo.td`'s `CALL` def emits `"EJSR $func,0"`
instead of `"JSR @$func,0"`; `EclipseISelLowering.cpp`'s `LowerCall` no
longer synthesizes a `"<callee>_SLOT"` name, just passes the callee's
own symbol through; `EclipseAsmPrinter.cpp`/`.h` drop the `CallSlots`
bookkeeping and the "indirect call jump table" `var` emission entirely.
`main`'s `_start` trampoline call is now `EJSR main,0`. The *indirect*
(function-pointer) call mechanism (`CALLIND`/`CALLIND_JSR`, entry #17)
is untouched — `EJSR` still can't take a register operand on real
hardware, so a runtime-computed callee still goes through the shared
`_scratch` word exactly as before; that mechanism was never a
page-zero *budget* problem in the first place (one shared word, not one
per call site).

**Verified**: `regress_hwstack.sh` byte-identical to the pre-change
baseline except for benign PC/incidental-disassembly shifts at each
`HALT`/timeout line (same pattern as entry #21) and the same four
pre-existing "Step expired" cases (`isr_c_test`, `test_fps_add`,
`test_fps_md`, `fps_dma_test`). Full c-testsuite: 197/220, PASS set
diffed directly against the pre-change baseline — identical, zero
regressions. `00216` (previously `COMPILE_FAIL` on page-zero exhaustion)
now compiles and assembles successfully and instead times out at
runtime — real forward progress from the freed page-zero budget, not
yet a pass on its own.

### 23. Global access via `ELEF`/`ELDA`/`ESTA`, eliminating the page-zero pointer-per-global scheme (changed design, verified) — plus a `reorder_asm.py` multi-word-instruction bug found and fixed alongside it

Follow-on to entry #22, applying the same real-extended-addressing
capability to the bigger page-zero cost in this project: the
pointer-per-global scheme entry in "Known limitations" (the one entry
above this whole numbered log describes, "Page-zero holds pointers, not
data") — `LEAGA` materializing a global's address by loading a
page-zero `var <name>_PTR = <name>` word, then `LDIND`/`STIND`
dereferencing through the shared `_scratch` word. That scheme itself
was a real fix for a real, earlier budget problem (see the README
section above) — but it still cost one page-zero word per distinct
global, regardless of size.

**Design**: `ELEF` ("extended load effective address") loads the
*address* an extended operand resolves to, not the word stored there —
confirmed empirically on eclipseemu (`ELEF 0,farvar` with `farvar` at
address 500 loaded `500` into AC0, not `farvar`'s stored value). `ELDA`/
`ESTA` load/store a word at an extended address directly. Three
`EclipseInstrInfo.td` defs now cover global access:
- `LEAGA` (unchanged name, changed implementation): now expands to a
  single `ELEF $dst,$addr` — no page-zero pointer word at all. Used
  wherever a global's *address* is needed as a value (`&global`, string
  literal/array decay, a function value), and as the address-
  materialization half of the existing runtime-offset composition with
  `LDIND`/`STIND` (a non-constant array index, an 8-bit extending/
  truncating access) — both unchanged.
- New `ELDAGA`/`ESTAGA` (real mnemonics `ELDA`/`ESTA`): match the
  common case of a *direct* load/store of a global's content with no
  runtime offset — one real instruction instead of `LEAGA`+`LDIND`/
  `STIND`'s three. New, more specific `Pat`s
  (`(load (EclipseWrapper tglobaladdr))` / `(store ..., (EclipseWrapper
  tglobaladdr))`) win under SelectionDAG's normal deepest-match
  preference over the older two-node composition, without disturbing
  any case that doesn't match exactly (extload/truncstore, a runtime
  offset) — those still fall through to the unchanged composition.
- A compile-time-constant offset (an array index, a struct field —
  already folded into the `GlobalAddress` operand's `Offset` field by
  `isOffsetFoldingLegal`, entry #18) is now folded directly into the
  emitted address text (`"target+N"`) instead of materialized as a
  separate constant word added at runtime via AC2: confirmed
  empirically that `dgasm`'s expression evaluator resolves
  `"symbol+integer"` to the symbol's own assigned address plus that
  integer at assemble time (`ELDA 0,farvar+2` correctly read the third
  word after `farvar`). This needs no extra instruction or page-zero
  word at all, and incidentally fixes a latent gap in the old `LEAGA`
  implementation, which performed the equivalent runtime add through
  AC2 without ever declaring AC2 as a clobbered register in TableGen.

Removed as a result: the `AddrSlots`/`addAddrSlot` (`*_PTR`) and
`OffsetSlots`/`addOffsetSlot` (`*_offN`) bookkeeping in
`EclipseAsmPrinter`, and their `var`-line emission in
`emitEndOfAsmFile` (now empty — nothing left to flush at module end).
`tconstpool` is untouched, per its existing comment: already one word
each, indirecting or extended-addressing it would add overhead with no
page-zero savings.

**A real regression found during verification, root-caused and fixed —
not in the backend itself**: the first full c-testsuite run after this
change showed exactly one regression from the entry #22 baseline,
`00150` (a global struct pointer with nested struct/pointer/array
fields) going from `PASS` to `TIMEOUT`. Traced with a step-by-step
`eclipseemu` trace (`PC`/`AC2`/`AC3` sampled every 15 steps) to a `JMP
0` trap partway through `main`, with `AC2`/`AC3` (the frame pointer)
still holding a plausible, unchanged value right up to the jump — ruling
out the classic return-address-corruption shape and pointing instead at
program data getting silently overwritten. Confirmed directly:
`eclipse-toolchain/reorder_asm.py`'s `compute_addresses` — which
replicates `dgasm`'s own pass-1 sequential address assignment, and which
`fix_stack_pointer` trusts to compute where the *real* end of the
program's data is so it can place `_STACKTOP` safely above it — assumed
every real instruction is exactly one 16-bit word (true for every
opcode this backend emitted before entries #22/#23; the file's own
comment said so explicitly). `EJSR`/`ELEF`/`ELDA`/`ESTA` are genuinely
2 words each, so any program with enough of them ahead of its trailing
bulk data accumulated a real, silent under-count — confirmed on `00150`:
`compute_addresses` computed the program's end at decimal 235, while
`dgasm`'s real, assembled output actually ended at decimal 243, an
8-word gap. `_STACKTOP` (`end_addr + 4096` margin) landed 8 words too
low, so the upward-growing hardware stack immediately overwrote the
tail of the program's own global data (`gs1`/the compound literal/`s`)
the moment `main`'s prologue and first locals were pushed — invisible on
small programs (not enough extended instructions ahead of the data to
accumulate a large enough gap to matter) and only surfacing once a
program had enough of them, which is exactly why the regression was a
single test, not many.

**Fix** (`eclipse-toolchain/reorder_asm.py`, synced to
`eclipse-package/eclipse-toolchain/reorder_asm.py`): a new
`EXTENDED_INSN_RE` matching the full real extended-addressing mnemonic
family (`EJSR`/`EJMP`/`ELEF`/`ELDA`/`ESTA`/`EISZ`/`EDSZ`/`ELDB`/`ESTB`
— only the first four are actually emitted by this backend today, the
rest included for the same family this project's own opportunity
research already confirmed exists), and `compute_addresses` now
advances the address counter by 2 for a matching line instead of
unconditionally by 1. Re-verified directly: with the fix,
`compute_addresses`'s own `end_addr` for `00150` now matches `dgasm`'s
real final deposited address exactly (both decimal 243).

**Verified**: `regress_hwstack.sh` byte-identical to entry #22's own
output except for the same benign PC/incidental-disassembly shifts, and
the same four pre-existing "Step expired" cases. Full c-testsuite (with
the `reorder_asm.py` fix in place): 197/220, PASS set diffed directly
against entry #22's own baseline — identical, zero regressions (`00150`
itself re-confirmed `PASS` again). Without the fix, this phase's backend
change alone regressed exactly `00150` and nothing else, from 197 to
196 — confirmed by diffing the two `PASS` sets directly, not just
comparing counts. Hand-verified end-to-end on eclipseemu, separately
from the c-testsuite: a global scalar load/store, a constant array
index, a struct field access (folded-offset `ELDA`), `&global`, and a
final runtime pointer dereference through the loaded address, all in
one program, produced the arithmetically correct combined result.
`00216` (page-zero-exhaustion `COMPILE_FAIL` before entry #22, `TIMEOUT`
after it) still does not pass after this phase — it compiles, assembles,
and runs further than before, but still ends in an unrelated `JMP 0`
trap; directly confirmed this is *not* a recurrence of the
`compute_addresses` bug above (its own computed `_STACKTOP` clears the
program's real end address by the full margin for this test) — some
other, pre-existing defect in that large/complex test, not investigated
further (out of scope for this change).

### 24. Hardware multi-bit shifts via `HXL`/`HXR`, replacing O(Amt) self-add/DIV sequences for the common cases

Every `<<`/`>>` on this backend was O(Amt) (or worse): `LowerShift`
(`EclipseISelLowering.cpp`) built `SHL` from a chain of up to `Amt`
self-adds, and `SRL`/`SRA` from a single `UDIV`/`SDIV`-by-`2^Amt` — cheap
*to write*, but `UDIVrr`'s post-RA expansion
(`EclipseInstrInfo.cpp::expandPostRAPseudo`) is actually a ~9-real-
instruction sequence (save AC2/the frame pointer to the hardware stack,
prime AC0/AC1/AC2, the hardware `DIV` itself, move the result out,
restore AC2) *regardless of the divisor's value*, since it's one
hardware division either way — so every `SRL`/`SRA`, even `x >> 1`, paid
that full cost. Real Eclipse S/140 hardware turns out to have genuine
multi-bit shift instructions that sidestep both costs for the common
case, confirmed directly on `eclipseemu` (not just the manual — see
below), that this backend simply wasn't using yet.

**Verification methodology**: hand-assembled `dgasm` probes (not
compiler-generated — this backend had no way to emit these instructions
before this change), loaded via `do <file>.simh` and single-stepped with
`step 1`/`e AC0` etc., the same way entries #22/#23 verified `EJSR`/
`ELEF`/`ELDA`/`ESTA`. `org 050` (octal), not `org 50` — see entry #22's
own note on this exact gotcha, still worth repeating: without the
leading zero `dgasm` parses it as *decimal*, landing code at a different
address than `dep PC 50` (octal) expects, silently executing zero-filled
memory that decodes as an infinite `JMP 0` self-loop.

**What was verified**: only `N=1` of `HXL`/`HXR`/`DHXL`/`DHXR` had been
checked before this change (in an earlier session, informally). This
entry checked `N=2,3,4` (and `N=0`) for all four, directly:

- `HXL ac,N` / `HXR ac,N` ("hex shift left/right"): shift accumulator
  `ac` by `N` *hex digits* — i.e. `4*N` bits — left/right, logical
  (zero-fill), in one instruction. Confirmed exactly: `AC0=1` then `HXL
  0,2` → `AC0=0400` (octal) `=256=1<<8`; `HXL 0,3` → `AC0=010000
  =4096=1<<12`; `HXL 0,4` → `AC0=0` (all 16 bits shifted out of a 16-bit
  accumulator — expected, and matches the *old* self-add loop's own
  `Amt>=16` behavior). `HXR` confirmed symmetrically on `AC0=0125715`
  octal `=0xABCD`: `HXR 0,2` → `0253` octal `=0x00AB` (`0xABCD>>8`);
  `HXR 0,3` → `012` octal `=0x000A` (`>>12`); `HXR 0,4` → `0` (`>>16`).
- `DHXL ac,N` / `DHXR ac,N`: the same shift on the 32-bit pair
  `AC(ac):AC(ac+1)` (`ac`=high word, `ac+1`=low word — matches the `N=1`
  convention verified earlier). Confirmed the same 4-bit-per-`N` rule
  *and* correct word-boundary crossing: `AC1=0,AC2=1` (representing
  `0x00000001`) then `DHXL 1,4` → `AC1=1,AC2=0` (`0x00010000` — the bit
  correctly moved from the low word into the high word, not lost or
  duplicated). `DHXR` confirmed the mirror case: `AC0=1,AC1=0`
  (`0x00010000`) then `DHXR 0,4` → `AC0=0,AC1=1`.
- `N=0` is **rejected by `dgasm` outright** for all four: `"Immediate out
  of range. Must be 1-4, got 0"` — a compile-time assembler error, not a
  silent no-op or garbage result. `LowerShift` never emits `N=0` as a
  result (skips the instruction entirely when there's no whole hex digit
  to shift).

All four are logical/zero-fill (no sign extension), matching the S/140
Programmer's Reference and the earlier `N=1` verification.

**Design** (`EclipseInstrInfo.td`): `HXL`/`HXR` defined exactly like the
existing `ALCrr` family (`ADD`/`SUB`/etc.) — tied `$dst = $src`,
accumulate-in-place, plain `AsmString` templates (`"HXL $dst,$n"`) that
need no custom `AsmPrinter` case at all, since the immediate operand
prints as a plain decimal integer (`EclipseAsmPrinter::printOperand`'s
`MO_Immediate` case) and `dgasm` accepts a small decimal `N` directly
(confirmed by the probes above — no octal needed for these small
immediates, unlike `org`). `DHXL`/`DHXR` are defined the same shape
(single `GPR` operand naming only the pair's high register, `ac`, with
`ac+1` as real hardware's implicit second operand) but **deliberately
not wired into any `Pat` or codegen path**: this backend has no
register-pair class (no other paired/32-bit machine-level operation
exists here — every `i32` op goes through a runtime libcall instead, see
`eclipse_rt.c`), and with only `AC0`/`AC1` ever handed to the register
allocator (`AC2`/`AC3` permanently reserved as frame pointer/return
address), a real pair operation would need the allocator to guarantee
`$dst`/`$src` land in two *specific, adjacent* physical registers, which
an ordinary tied `GPR` operand doesn't express or enforce. Using them
safely needs either a dedicated paired-register class or hand-emitted
code against hardcoded physical registers outside the normal allocator —
flagged as real follow-up work, not attempted here given this task's
time budget and this project's own stated preference for a smaller,
verified win over a larger, rushed one.

**`LowerShift` rework** (`MVT::i16`, constant amounts only — unchanged):

- `SHL`: `Amt` splits into a whole-hex-digit part (`Amt/4`, always ≤3
  since this rework caps the effective amount at 16 exactly like the old
  loop's `I<16` cap, and `HXL`'s own `N` maxes out at 4 anyway) — one
  `HXL` call — plus the existing self-add chain for the `Amt%4`
  remainder (0-3 bits, no cheaper option exists: this ISA has no
  finer-than-4-bit hardware shift). `x << 15` drops from 15 chained
  `ADDrr` to one `HXL` (12 of the 15 bits) + 3 `ADDrr`; any `Amt>=16`
  (all bits shifted out) drops from up to 16 `ADDrr` to a single `HXL`.
  `Amt<4` is untouched — no `HXL` emitted, byte-for-byte the old
  self-add-only behavior, confirmed no regression there.
- `SRL`: any `Amt` that's an exact multiple of 4 now uses a single `HXR`
  instead of the full `UDIVrr` expansion — replacing ~9 real instructions
  with 1 for the common `>>4`/`>>8`/`>>12` byte/nibble-extraction case.
  A non-multiple-of-4 `Amt` still goes through `UDIV`: `HXR` for the
  multiple-of-4 part plus a *separate* `UDIV` for the 1-3-bit remainder
  would cost strictly *more* instructions than today's single
  `UDIV`-by-`2^Amt`, since `UDIV`'s expensive save/restore/`DIV`
  sequence has to run in full regardless of how small its divisor is —
  confirmed by reading `expandPostRAPseudo` directly rather than assumed.
- `SRA` deliberately **left unchanged**: `HXL`/`HXR`/`DHXL`/`DHXR` are
  logical, wrong for a negative operand's sign-preserving shift. The
  classic `(x<0) ? ~(~x >>u n) : (x >>u n)` trick could let `SRA` reuse
  the `SRL` win above, but `LowerShift` has already had one real,
  previously-fixed register-corruption bug from a chained-divisor `SRL`
  optimization (see this function's own comment, and the "regmd always
  reads 15" section below for the original bug this fixed) — judged not
  worth stacking a second risky rework on the same fragile function in
  one change; left as documented future work.
- `MVT::i32` (`long`) shifts are untouched: still decomposed into `i16`-
  pair operations by the generic type legalizer (no register class for
  `i32` here at all). A direct `DHXL`/`DHXR`-based `i32` `Custom`
  lowering was considered (this task's stated stretch goal) but hits the
  same register-pair problem as `DHXL`/`DHXR`'s own TableGen design
  above, with *zero* spare scratch register once the pair is committed
  (this backend has exactly two allocatable registers, and the pair
  would consume both) — not pursued.

**`eclipse_rt.c` investigated, not changed**: `sf_shr`/`sf_shl` (used by
`sf_add`'s exponent-difference mantissa alignment, and the float↔int
conversion/`printf` fractional-digit-extraction paths) are runtime-
*variable*-count 32-bit shifts, done one compile-time-constant-amount
bit at a time in a C `while` loop — specifically because
`ISD::SHL_PARTS`/`SRL_PARTS` (a true variable-amount 32-bit shift) has
no lowering here (confirmed by that function's own existing comment:
"Cannot select: ... srl_parts ..."). Each individual loop-body shift is
already the cheap single-bit case this change's `SHL`/`SRL` rework
doesn't touch (`Amt=1` never reaches the new `HXL`/`HXR` path — `Quads`
is always 0), so the loop's real cost is per-iteration overhead
(compare/branch/decrement), not the shift instruction itself — this
change does not meaningfully speed up that hot loop. A real win would
need an algorithmic change (count the shift amount up front, one
`DHXL`/`DHXR` call for the bulk, single-bit correction for the
remainder), which needs inline asm against hardcoded physical registers
for the same reason `DHXL`/`DHXR` aren't wired into general codegen
above — a real, separate, riskier change to a file this project has
already had to split into multiple smaller functions once just to fit
frame-offset limits (see `sf_add`'s own header comment). Not attempted;
recorded here as a legitimate, specific, unpursued follow-up rather than
silently left for someone to rediscover.

**A pre-existing, unrelated bug found (not caused) while writing this
change's test coverage**: a battery C probe exercising ~20 different
shift expressions on locals in one `main()` printed a `long` local's low
16-bit word as `0` (`123456789` read back as `123404288` — the high word
correct, only the low word zeroed) — but *only* inside that specific
larger function; an isolated 4-line reproduction of the exact same
constant and shift compiled and ran correctly. Confirmed via `git
stash`/rebuild that this reproduces byte-for-byte on the *unmodified*
baseline too (same wrong output, same repro), so it predates and is
unrelated to this change — most likely adjacent to the large-stack-frame
class of bug entry #21 already fixed one instance of, but not confirmed
as the same root cause and not investigated further here (out of this
task's scope). Worked around in this change's own test coverage by
splitting that one case into its own small function, which does not
reproduce it.

**Verified**:
- `eclipseemu`, by hand: all 4 instructions × `N` in `{1,2,3,4}` (`N=1`
  previously verified, `N=2,3,4` newly verified this change) plus `N=0`
  rejection, per the "What was verified" section above.
- Assembly inspection: generated code for constant shift amounts
  0,1,3,4,5,7,8,12,15 confirms the exact expected instruction sequence
  (`HXL`/`HXR` alone, `HXL`+remainder `ADDrr`, or the unchanged `UDIV`
  sequence) for each case, by direct `.s` inspection against the
  expected `Quads`/`Rem` split.
- Two hand-written `eclipseemu`-run C probes: `i16`/`i32` `SHL`, `i16`
  `SRL` (multiple-of-4 and non-multiple), `i16` `SRA` (positive and
  negative), `i32` `SRL`, `i32` `SRA` (positive and negative) at amounts
  0/1/3/4/5/7/8/12/15 (`i16`) and 0/1/3/4/8/15/16/20/31 (`i32`) — all
  correct, byte-for-byte identical to independently (Python-)computed
  expected output.
- `regress_hwstack.sh`: identical to a freshly-rebuilt baseline (stashed
  this change, rebuilt `llc`, re-ran) except for the same benign
  PC/incidental-disassembly shifts at each `HALT` line seen in entries
  #22/#23, and the same 4 known/expected `Step expired` cases
  (`isr_c_test`, `test_fps_add`, `test_fps_md`, `fps_dma_test`).
- Full c-testsuite: 197/220, `PASS` set diffed directly against the same
  freshly-rebuilt baseline — identical, zero regressions, zero
  incidental new passes.

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

### 25. Follow-up on entry #24's "pre-existing, unrelated bug" — reproduced and narrowed, root cause not fully pinned down, not fixed

Entry #24 found (but explicitly didn't investigate) a bug where a
`long` local's low 16-bit word read back as `0` inside a large `main()`
with ~20 other locals, but not in an isolated 4-line reproduction, and
speculated it was "most likely adjacent to the large-stack-frame class
of bug entry #21 already fixed one instance of". The original repro
program itself wasn't preserved. This entry rebuilt a reproduction from
scratch, confirmed the bug on real `eclipseemu`, and ruled out entry
#21's own mechanism as the cause — but did not reach a confirmed single
root cause, and made no code change.

**Reproduction** (reliable, confirmed on `eclipseemu`): a `main()` with
40 `int` locals, one `long`, 5 more `int` locals, a loop-free sum over
all of them, a `long shifted = L << 1;`, then a call to *any* function
that takes **two or more** parameters, then `printf("shifted=%ld\n",
shifted)`. The printed value's low word is `0`
(`shifted=246874112` instead of the correct `246913578` — high word
`03767` octal correct both times, low word `0` instead of `0111752`
octal) — the exact same symptom shape entry #24 described.

```c
extern int printf(const char *, ...);
__attribute__((noinline)) void twoarg(int x, int y) { (void)x; (void)y; }
int main(void) {
    int b0 = 1;   int b1 = 2;   /* ... 40 int locals total, b0..b39 = 1..40 ... */
    int b39 = 40;
    long L = 123456789L;
    int a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5;
    int sum = 0;
    sum += b0 + b1 + /* ... every b and a local ... */ + a4;
    long shifted = L << 1;
    twoarg(10, 20);                    /* any call pushing >=2 words */
    printf("shifted=%ld\n", shifted);  /* prints 246874112, should be 246913578 */
    return 0;
}
```

**What was ruled out, confirmed by direct experiment (not assumed):**

- **Not entry #21's mechanism.** Entry #21 is about a stack object's own
  FP-relative displacement exceeding the hardware's 8-bit indexed-
  addressing range (-128..127), triggering `eliminateFrameIndex`'s
  address-materialize-through-`_scratch` fallback. In every variant of
  this repro, `shifted`'s and `L`'s own displacements stayed at 88-107
  words — comfortably in range, direct `LDFI`/`STFI`, no fallback ever
  taken for either object itself. `%const` values of `128` do appear in
  the constant pool in *both* passing and failing variants (something
  else in the frame does cross the boundary either way), so mere
  co-occurrence of an entry-#21 fallback elsewhere in the function is
  not sufficient by itself to explain the difference between a passing
  and failing variant.
- **Not about the value, the shift, or `L` itself.** `L` alone always
  printed correctly in every variant tried, including ones where
  `shifted` printed wrong. The corruption is specific to `shifted` — a
  *second*, compiler-synthesized `long` stack object — and specifically
  to its low word (word 1 of 2; the high word, word 0, was correct in
  every single repro run).
- **Not variadic-ness.** First hypothesis, disproven directly: a
  hand-written variadic function (`void myva(int n, ...)`) reproduced
  it, but so did a perfectly ordinary, non-variadic two-parameter
  function (`twoarg` above) — and a non-variadic *one*-parameter
  function (`onearg(int)`) never did, even called from a function with
  an intentionally huge callee-side frame of its own (a 40-local
  `bigcall(int)`, to rule out "large callee frame" as the real trigger
  independent of argument count). The determining factor empirically is
  **how many words the call pushes as arguments (>=2), not whether the
  callee is variadic or how large the callee's own frame is.**
- **Not caused by any call at all** — confirmed a plain call with zero
  or one pushed argument (`noop()`, `printf("hi\n")` with no `%`
  arguments, `onearg(42)`, `bigcall(42)`) never corrupts `shifted`,
  regardless of frame size up to the tested range. Printing `shifted`
  as the *first* statement after computing it (before any call at all)
  is always correct.
- **Frame-size-dependent**, separately from the >=2-argument-push
  requirement: with the exact repro shape above, 38 "before" locals
  (`main`'s own `SAVE 134`) never reproduces it; 40 "before" locals
  (`SAVE 138`/`SAVE 136` depending on which call variant) reproduces it
  reliably. Both conditions (>=2 pushed words, and a large enough
  enclosing frame) are independently necessary in every variant tested.

**What the low word's address computation actually looks like**
(confirmed by reading `EclipseISelLowering.cpp` and cross-checking
against `-stop-before=prolog-epilog`/`-stop-after=prolog-epilog` MIR
dumps): a stack-resident `i32` (`long`) load/store always splits into
two `i16` accesses. `EclipseISelDAGToDAG.cpp`'s `ISD::LOAD`/`ISD::STORE`
handling only matches a *bare* `FrameIndex` base pointer directly to
`LDFI`/`STFI` — so only word 0 (the high word, at the object's own
offset) ever gets that direct match. Word 1 (the low word, at
`FrameIndex + 1` word) is `ADD(FrameIndex, 2-bytes)`, which
`PerformDAGCombine` (same file, the large comment starting around its
`ISD::ADD` check) rewrites into `EclipseISD::WORD_ADD(LEAFI-materialized
base, EclipseISD::HALVE(2, 2))` — i.e. the low word's real address is
computed at runtime (`FP + object-offset`, then `+1` from a runtime
halving of the generic legalizer's byte-granular `+2`), routed through
the single shared page-zero `_scratch` word as the final indirect
load/store address, the same mechanism `eliminateFrameIndex`'s own
out-of-range fallback and `emitIndirectMem`'s general pointer
dereference also both use. The high word never goes through any of
this — it's always a plain, direct, compile-time-resolved `LDFI`/`STFI`
displacement. This asymmetry (only the low word needs a *runtime*
address computation at all) lines up exactly with the symptom (only the
low word is ever wrong).

**Where this investigation stalled:** `-stop-before=prolog-epilog` and
`-stop-after=prolog-epilog` MIR dumps of both a passing (38-local) and
failing (40-local) variant were traced instruction-by-instruction by
hand, including every spill slot touching the low word's address or
value and every `%stack.N`/real-offset pair involved in computing it
(`STFI`/`LDFI` reload immediately preceding each `STABS`/`STABSI &
_scratch` pair). In both variants the generated `MachineInstr` sequence
is self-consistent and structurally identical in shape — no dropped
reload, no stack-slot reused by two overlapping live ranges was found
by this method, in either variant, despite ~15 separate spill slots
checked this way. (Aside: while tracing a *different*, hand-written
variadic-function repro during this same investigation, one push
argument's value was observed being read from a register that had last
been written by an unrelated earlier computation rather than a fresh
reload from its own spill slot — a real anomaly, but the `twoarg`
non-variadic repro above reproduces the main `shifted` bug without
exhibiting that specific symptom in its own MIR, so this looks like a
second, separate defect rather than the explanation for this one; not
investigated further, not confirmed as a real bug on its own, flagged
here only so it isn't silently lost.) Direct `eclipseemu` single-step
inspection of the runtime address `shifted`'s low word resolves to, and
of the shared hardware-stack-pointer word (page-zero address `040`,
used both by `eliminateFrameIndex`'s fallback / `emitIndirectMem` /
`PerformDAGCombine`'s `_scratch` indirection *and*, per entry #11's
design, by `LowerCall`/`emitPushPop`'s argument-pushing and by real
`SAVE`/`RTN`) was attempted but did not converge on a clear answer
within this session's time budget — the probed low-word address read
back `0` at every checkpoint taken, including checkpoints that should
have been *before* the corruption (making the specific probe
inconclusive rather than confirmatory; likely too coarse a step
granularity relative to the handful of instructions where the actual
write and any clobber would happen).

**Leading (unconfirmed) hypothesis**, offered for whoever picks this up
next: `_scratch` and the page-zero hardware-stack-pointer word are both
single, shared, unsynchronized page-zero locations that multiple,
independently-generated instruction sequences funnel a runtime address
through (the low-word `WORD_ADD`/`HALVE` materialization, entry #21's
`eliminateFrameIndex` fallback, `emitIndirectMem`'s general pointer
dereference, and — for the stack-pointer word specifically —
`emitPushPop`'s argument marshaling and real `SAVE`/`RTN`). None of
these carry an explicit `MachineMemOperand`/chain relationship to each
other, so nothing prevents two of them from interleaving unsafely if
the surrounding code shape and register/frame pressure line up right —
which would explain why this needs both a large-enough frame *and*
multiple pushed argument words to surface (more concurrent demand on
the same page-zero words), while never needing the offending object's
*own* displacement to exceed entry #21's 8-bit range. This was not
verified to the same standard as entries #21/#24's own findings — no
fix was attempted, and none should be attempted without first
confirming this mechanism directly (e.g. instrumenting `_scratch`'s
value across the push sequence, or trying a build with a second,
independent scratch word for the `WORD_ADD`/`HALVE` path specifically
to see if that alone makes the repro above pass).

**Verified**: reproduces reliably and byte-for-byte on real
`eclipseemu` (not just "looks wrong"), both with the repro above and
with the original hand-written variadic-function variant that led to
finding the >=2-pushed-words condition. No backend code was changed;
`regress_hwstack.sh` and the c-testsuite baseline were not re-run
because nothing in the backend was touched this session.

### 26. Follow-up on entry #25 — root cause confirmed, fixed with a second scratch word

Continuation of entry #25 (read that entry first — this picks up exactly
where it stalled). Entry #25's own leading hypothesis is confirmed: the
shared page-zero `_scratch` word was being reused, unsynchronized,
between two independently-generated mechanisms that can land in the same
few statements of a large function — `PerformDAGCombine`'s `WORD_ADD`/
`HALVE` low-word address materialization, and
`EclipseRegisterInfo::eliminateFrameIndex`'s own out-of-range fallback
(entry #21). Fixed with entry #25's own first-suggested experiment: a
second, independent page-zero scratch word (`_scratch2`) dedicated to
the `WORD_ADD`/`HALVE` path alone.

**Confirming the root cause, not just the symptom.** Rather than jumping
straight to the fix-and-test cycle, this session first dumped MIR at
`-stop-before=prolog-epilog` and `-stop-after=prolog-epilog` for entry
#25's own repro (reproduced fresh, byte-for-byte identical symptom:
`shifted=246874112` instead of `246913578`, low word `0`) and hand-
traced every `_scratch` touch in the region between computing `shifted`
and the corrupted post-call read. Two concrete findings came out of that
trace, both confirming entry #25's hypothesis was pointed at the right
mechanism (even though its own single-step attempt hadn't converged):

- `LDABS`/`STABS`/`LDABSI`/`STABSI` — the instructions every one of
  these mechanisms is built from — carry no `mayLoad`/`mayStore` in
  `EclipseInstrInfo.td` and no `MachineMemOperand` at their `BuildMI`
  call sites anywhere in `EclipseISelLowering.cpp` or
  `EclipseRegisterInfo.cpp` (confirmed by grepping both files and by
  checking the generated `EclipseGenInstrInfo.inc`: they get
  `MCID::UnmodeledSideEffects` from TableGen's own pattern-less-
  instruction inference instead, not a real load/store flag). This
  means nothing downstream of instruction selection has any principled
  way to know two `STABS ...,&_scratch` instructions from *different*
  original mechanisms touch the same memory location — there is no
  chain, no aliasing info, nothing; entry #25's "no explicit
  `MachineMemOperand`/chain relationship" line was exactly right.
- In the actual post-`prolog-epilog` MIR for the large, failing repro,
  `eliminateFrameIndex`'s fallback (entry #21) fires *constantly* — the
  frame is large enough that nearly every stack access beyond the first
  ~127 words needs it, and its own documented sequence
  (`STABS $val,&_scratch` to stash a store's value up front, unrelated
  offset-decomposition arithmetic in between, `LDABS &_scratch` to
  reload it, then `STABS $addr,&_scratch`/`STABSI $val,&_scratch` for
  the real indirect access — see entry #21) is *itself* just one more
  independent, unchained user of the same word, no different in kind
  from the `WORD_ADD`/`HALVE` path. With a 40-local frame plus a
  multi-argument call in between, both mechanisms end up needing
  `_scratch` within the same handful of statements — exactly the
  "densely interleaved, nothing to keep one from clobbering the other"
  scenario entry #25 described, even though *this specific* function's
  compiled instruction order didn't happen to physically interleave the
  two mechanisms' own instruction pairs (each individual `STABS`+
  indirect-access pair stayed adjacent in this repro's output — no
  reordering pass tore one apart). The real exposure is structural, not
  a one-off scheduling accident: two totally independent generators
  share one word with no coordination at all, and this repro's specific
  code shape (large frame, multi-word `long`, a value that must survive
  a call and therefore gets register-allocator-spilled to yet another
  out-of-range slot, needing its *own* `eliminateFrameIndex` fallback
  right where the `WORD_ADD`/`HALVE` path also needs `_scratch`) is
  simply the first one that happened to line the two up badly enough to
  matter.

**Fix**: a second, independent page-zero word, `_scratch2`
(`EclipseAsmPrinter.cpp`, emitted immediately after `_scratch`, still
swept into `reorder_asm.py`'s fixed preamble the same way), used *only*
by a load/store whose address is exactly an `EclipseWordAdd` node —
i.e. only the `PerformDAGCombine`-driven low-word/runtime-GEP
materialization entry #25 already identified, never `eliminateFrameIndex`'s
fallback, `emitIndirectMem`'s other (non-`WORD_ADD`) uses, `emitPushPop`,
or `emitCallIndirect`, all of which keep using the original `_scratch`
exactly as before. Implemented as a new pseudo-instruction pair in
`EclipseInstrInfo.td`, `LDINDW`/`STINDW`, matched by a strictly deeper
Pattern than `LDIND`/`STIND`'s plain `GPR:$addr` (`(load (EclipseWordAdd
GPR:$base, GPR:$off))` and the store/extload/truncstore equivalents) so
SelectionDAG's normal complexity-based preference — the same mechanism
`ELDAGA`/`ESTAGA` already rely on in this file — picks it automatically
whenever the address is `WORD_ADD`-shaped, with no change needed to
`PerformDAGCombine` itself. A new custom inserter,
`emitIndirectMemWordAdd` (`EclipseISelLowering.cpp`, right next to
`emitIndirectMem`), adds `$base`+`$off` into a fresh virtual register
(an ordinary tied-operand `ADDrr`, same idiom `LEAFI`'s own expansion
uses) and then indirects through `_scratch2` exactly like
`emitIndirectMem` does through `_scratch`. This is purely additive: no
existing pseudo, pattern, or custom-inserter function was modified, only
new ones added alongside them, and `eliminateFrameIndex` itself was not
touched at all.

**Confirmed causally, not just correlationally.** Recompiling entry
#25's exact repro after the fix and diffing the generated assembly shows
`_scratch2` appearing at precisely the sites this fix targets — the
low-word read/store of `L` and of `shifted`, including the specific
post-`twoarg`-call read that used to come back `0` — while every other
`_scratch` use in the same function (the dense run of
`eliminateFrameIndex` fallbacks around it) is untouched and still on the
original `_scratch`. Running the fixed build on real `eclipseemu`:

```
shifted=246913578
```

— correct (high word `03767` octal, low word `0111752` octal, matching
entry #25's own expected value), where the pre-fix build printed
`shifted=246874112` (low word `0`) with byte-identical source and
identical repro steps otherwise. Isolating the change to `_scratch2`
alone (nothing else in the backend touched) makes this a genuine before/
after causal test, not just "a fix happened to be near the bug."

**Full regression, byte-identical to baseline**: `regress_hwstack.sh`'s
15 examples — 11 `HALT` with correct output, and exactly the same 4
pre-existing `Step expired` cases as baseline (`isr_c_test`,
`test_fps_add`, `test_fps_md`, `fps_dma_test`, all unrelated to this
change — FPU/device-polling limitations noted in earlier entries), no
new failures and no output changes on any passing example.
c-testsuite: 197/220, identical to the pre-fix baseline re-measured this
same session (same 23 non-passing test IDs before and after: `00040`,
`00041`, `00104`, `00157`, `00163`, `00166`, `00168`, `00174`, `00175`,
`00178`, `00179`, `00180`, `00187`, `00189`, `00195`, `00203`, `00204`,
`00205`, `00212`, `00216`, `00217`, `00218`, `00220`) — no regressions,
no new passes (expected: this bug's repro shape, a specific combination
of frame size and call-argument count, doesn't happen to be exercised by
c-testsuite's own test set).

**Page-zero cost**: exactly one word (`_scratch2`), the minimum this
fix could cost — no blanket "give every mechanism its own word,"
just the one confirmed collision.

(This session also worked concurrently in the same tree as an unrelated
opt-in hardware-floating-point change — `Eclipse.td`, `EclipseSubtarget.
{h,cpp}`, `RuntimeLibcalls.td`, and part of `EclipseISelLowering.{h,cpp}`
had another agent's uncommitted work in them throughout; this entry's
commit stages only the hunks described above, verified with `git diff
--cached` before committing, leaving the concurrent work uncommitted for
its own session to land.)

### 27. Real Eclipse S/140 hardware FPU support (`--hwfloat`), opt-in and additive — float add/subtract via FAS/FSS instead of software libcalls

This is the "unrelated opt-in hardware-floating-point change" entry #26
mentioned working concurrently alongside. Background: this target has
implemented `float`/`double` entirely in software since the "soft
float" work (see `SOFT_FLOAT_NOTES.md`) — every `+`/`-`/`*`/`/` is a
real C function call, measured at roughly 5,000 real machine
instructions per single `float` multiply. `EclipseISelLowering.h`'s own
header comment used to say plainly that this backend "does not
hand-encode ... Eclipse FPU opcodes" because "nothing here would be
empirically verified against real hardware the way eccc's ISA facts
are." This entry changes that, but narrowly and only opt-in: real
Eclipse S/140 hardware FPU instructions now implement `float`
add/subtract when a program is compiled with a new `--hwfloat` flag;
every invocation without that flag — which is every existing use of
this toolchain — is byte-for-byte unaffected, verified below.

**1. The hardware is real, and genuinely separate from the already-
documented FPS100.** This project's Background section documents an
*external* FPS100 I/O-device coprocessor at device code `054` that
`eclipseemu` cannot simulate at all (device `054` isn't implemented),
making that whole path unverifiable except on real hardware. The
Eclipse S/140 CPU turns out to *also* have a genuine CPU-native FPU —
real instructions, a real register file (`FPAC0`-`FPAC3`, readable via
eclipseemu's `e FPAC0` etc., distinct from the integer `AC0`-`AC3`) —
that `eclipseemu` *does* simulate (partially — see point 3). Confirmed
`dgasm -t eclipse_s140` recognizes `FAD`/`FSD`/`FMD`/`FDD` (double-
precision add/sub/mul/div), `FAS`/`FSS`/`FMS`/`FDS` (single-precision),
`FLDD`/`FSTD`/`FLDS`/`FSTS` (load/store), `FCMP`, `FMOV`, `FNEG`,
`FEXP`, `FLAS`/`FFAS` (int↔float conversion) as real mnemonics; many
plausible-looking alternates (`FSB`, `FDV`, `FLD`, `FST`, `FIX`, `FLT`,
`FABS`, `FCMPS`, `FCMPD`, ...) are rejected outright. Cross-cautionary
finding from the same probing session: a fifth-seeming-plausible
mnemonic, `DLSH`, assembles fine but is a genuine silent no-op in
`eclipseemu` — a direct instance of this project's own standing
warning ("not everything `dgasm` accepts is necessarily implemented by
`eclipseemu`") that motivated verifying every instruction end-to-end
below rather than trusting assembly success alone.

**2. Number format: IBM System/360-style hex float (reverse-
engineered, not assumed).** Secondhand web research suggested Nova/
Eclipse-family FPUs follow the IBM S/360 convention (sign bit + 7-bit
exponent in excess-64 notation over powers of *16* + a mantissa
normalized to a hex-digit/4-bit boundary, not IEEE's single-bit
normalization) — plausible but unconfirmed going in. Verified directly:
independently derived the encoding for `1.0`/`2.0`/`3.0`/`0.5`/etc.
from the hypothesis (`1.0 = 0x41100000`, matching the well-known real
S/370 constant), then hand-assembled a probe (`FLDS 0,val1_hi` /
`FLDS 1,val2_hi` / `FAS 0,1` / `FSTS 1,result_hi`, `org 050`, single-
stepped and inspected via `e FPAC0`/`e FPAC1` in `eclipseemu`) and
confirmed the hardware computes bit-exact correct sums for ~14 value
combinations, including negative operands and results (`-0.125+-0.875
= -1.0`, `1000.0+-999.0 = 1.0`, `-1.0+1.0 = 0.0`, ...). The hypothesis
is confirmed, not merely plausible.

**Real dgasm gotcha found along the way, unrelated to the FPU itself:**
`var` octal literals need an explicit leading `0` digit to be parsed as
octal at all (C-style convention) — a value whose top bit is set (any
negative-signed word, e.g. a negative float's high word) formatted as
a bare 6-digit octal string with no natural leading zero (e.g.
`140460`) gets silently re-parsed as *decimal* instead, producing a
completely different bit pattern (`140460` intended as octal
`0xC130` came back as `022254` — decimal `100000`'s mod-65536 wraparound
`34464`, which *is* `0103240` octal — confirmed by testing a battery of
`var x = <literal>` / `LDA 0,x` round-trips: every value below
`0100000` (no natural ambiguity) round-tripped correctly; every one at
or above it silently corrupted, and adding the leading `0` fixed every
case). This produced a very convincing false trail early on — a run of
`FAS` probes with negative operands looked exactly like broken hardware
add/subtract until this was isolated with a plain integer `LDA`/`STA`
round-trip that had nothing to do with the FPU at all.

**3. Multiply/divide are NOT usable on `eclipseemu` — probed and
confirmed broken there, not just "not attempted."** `FMS`/`FDS` (and
the FPAC-by-memory form, `FMMS`) assemble and execute (real PC
advancement, no crash) but always leave the destination `FPAC` at
exactly zero regardless of operand values — confirmed with `2×2`,
`1×1`, `2×3`, `2×-2`, `8÷2`, `1÷1`, and both `FMS 0,1`/`FMS 1,0`
operand orderings, every one landing on hard zero. This is the same
class of finding as `DLSH` above: real hardware per the manual, not
correctly simulated on this specific `eclipseemu` build. `FLAS`/`FFAS`
(int↔float conversion) assembled but showed operand-encoding-dependent,
inconsistent behavior across otherwise-equivalent invocations (e.g.
`FFAS 1,0` returning a correct result in one context and nothing in
another, depending on which physical `FPAC` actually held live data) —
not trusted, left unused. Neither gap blocks this entry's scope: only
`ADD_F32`/`SUB_F32` are hardware-accelerated; `MUL_F32`/`DIV_F32` and
int↔float conversion stay on the existing software libcalls
unconditionally, exactly as the task anticipated as an acceptable
fallback for exactly this situation.

**4. Design: alternate libcall implementation, not SelectionDAG-level
Custom lowering.** `float` has no register class on this backend at
all (`MVT::f32` is never legal — see `EclipseISelLowering.h`), so a
genuine Custom-lowered `ISD::FADD`/`ISD::FSUB` was never really on the
table: LLVM's type legalizer "softens" any operation on an illegal
float type into a libcall *before* per-operation legality is ever
consulted, and building a real `f32` register class (plus the register-
pair allocation machinery this backend has explicitly avoided even for
its existing `i32` support) was assessed as materially higher-risk than
needed. Instead: a new `hwfloat` `SubtargetFeature` (`Eclipse.td`,
`FeatureHWFloat`, off by default) swaps which `RTLIB::LibcallImpl`
`ADD_F32`/`SUB_F32` resolve to — `__addsf3_hw`/`__subsf3_hw` (new,
hand-written-assembly alternates, `rt/eclipse_hwfloat.s`) instead of
the existing software `__addsf3`/`__subsf3` — set in *two* places
(`EclipseISelLowering.cpp`'s constructor and
`EclipseSubtarget::initLibcallLoweringInfo`, exactly like every other
libcall this backend wires up — see that method's own comment for why
both copies are needed) based on `STI.hasHWFloat()`. This exactly
mirrors a real, existing upstream LLVM pattern: `RuntimeLibcalls.td`
already has ARM's own `__addsf3vfp`/`__subsf3vfp`/etc. — alternate
libcall implementations for an optional hardware-FPU path, selected the
same way. `__addsf3_hw`/`__subsf3_hw` needed adding to
`RuntimeLibcalls.td` itself (a small, additive, low-risk change — two
new `def`s next to the existing `__addsf3`/`__subsf3`, following that
exact file's own established pattern).

Confirmed the feature genuinely only changes what it should:

```
$ llc -mtriple=eclipse-dg-none -filetype=asm sanity_add.ll   # no -mattr
	EJSR __addsf3,0
	EJSR __subsf3,0
	EJSR __mulsf3,0
$ llc -mtriple=eclipse-dg-none -mattr=+hwfloat -filetype=asm sanity_add.ll
	EJSR __addsf3_hw,0
	EJSR __subsf3_hw,0
	EJSR __mulsf3,0                     # unchanged -- no hardware multiply
```

— `diff`ing the two `.s` files shows *only* those two `EJSR` targets
change; every other instruction is byte-identical.

**5. `EclipseSubtarget`'s member-construction order needed fixing for
the feature bit to be visible in time.** `TLInfo` (`EclipseTargetLowering`,
which reads `STI.hasHWFloat()` in its own constructor to pick the
libcall) is constructed in `EclipseSubtarget`'s member-initializer
list — but `ParseSubtargetFeatures` (which sets `HasHWFloat`) used to
run from the constructor *body*, always *after* every member is already
constructed. `TLInfo` would have always seen `HasHWFloat == false`
regardless of `-mattr=`. Fixed with the standard LLVM idiom other
subtargets use for the same problem: an `initializeSubtargetDependencies`
helper that runs `ParseSubtargetFeatures` first and returns `*this`,
called from the *first* member's own initializer (`InstrInfo`'s) so it
completes before any later member — `TLInfo` included — is constructed.

**6. Hand-written assembly, and the calling-convention gotchas that
came with it.** The LLVM backend has no SelectionDAG support for
emitting `FLDS`/`FAS`/`FSTS` (per point 4) and Clang has no inline-asm
support for this target, so `__addsf3_hw`/`__subsf3_hw` are hand-
written directly against the real ISA (`rt/eclipse_hwfloat.s`) and
textually appended to `llc`'s own output by `eclipse-cc` (only when
`--hwfloat` is passed) before `reorder_asm.py` runs — safe because
`reorder_asm.py` is pure line-oriented text manipulation, agnostic to
where a line originally came from. Making a hand-written function
correctly *callable* by real compiler-generated code (and able to
itself call real compiled C helpers — see point 7) needed the calling
convention nailed down precisely, and two non-obvious parts of it were
gotten wrong on the first attempt, both found via hand-assembled
verification harnesses (never assumed from reading `LowerCall`'s
source):

- **Page-zero addressing.** `reorder_asm.py`'s `PZ_VAR_RE` only hoists
  `var` lines matching the *compiler's own* pointer-slot/constant-pool
  naming (`*_SLOT`/`*_PTR`/`*_offN`/`CPI<n>_<m>`) into page-zero; this
  file's own `__hwf_*` scratch words are ordinary "bulk" data left
  wherever they land — far past page-zero's 0-255 range on any
  nontrivial linked program. A plain `LDA`/`STA` can only reach
  page-zero; confirmed empirically ("Address out of range. Got 5368,
  should be 0-255") the first time this file used them against its own
  scratch. Fixed by using `ELDA`/`ESTA` (the same extended/absolute
  addressing the compiler's own code already uses for every other bulk
  global) for every access to `__hwf_*`. `FLDS`/`FSTS` themselves need
  no such fix — confirmed via the same probe that they reach the full
  address space natively (no assembler complaint against a `__hwf_*`
  operand thousands of words past page-zero), consistent with them
  being genuine 2-word extended-addressing instructions in their own
  right.
- **Word order and argument order, two separate and easy-to-conflate
  gotchas.** (a) A 32-bit *value's* two words are pushed/read LOW word
  first, HIGH word second (parameters) — the opposite of the natural-
  looking assumption — while a *return* value comes back the other way,
  AC0 = HIGH, AC1 = LOW. Confirmed with a dedicated, assumption-free
  probe: two trivial C functions, `get_hi16(x)`/`get_lo16(x)`, called
  with two distinguishable pushed words (`0xAAAA` then `0xBBBB`) —
  `get_hi16` came back `0xBBBB` (the *second*-pushed word), `get_lo16`
  came back `0xAAAA` (the *first*), unambiguously fixing which is
  which. (b) For a *multi-argument* call, arguments are pushed
  **right-to-left** — `SOFT_FLOAT_NOTES.md` already said this about the
  software stack in general, but it's easy to read past as a detail
  that wouldn't matter for a 2-argument function specifically; it does.
  For `f(a, b)`, the *second* source argument (`b`) is pushed first
  (lands at `FP-8`/`FP-7`), the *first* (`a`) second (`FP-6`/`FP-5`).
  Confirmed empirically by deliberately reversing a hand-assembled test
  harness's push order for a direct call to `__subsf3_hw` and
  reproducing the *exact* same wrong-sign bug described next.

Getting (b) backwards doesn't fail loudly. `__addsf3_hw` is commutative
(`a+b == b+a`), so which physical slot is "a" and which is "b" is
invisible there — every add-only test passed on the first try. It only
surfaced as a bug in `__subsf3_hw`, which silently computed `b-a`
instead of `a-b` for every call with two genuinely different operands
that reached this code path at all — and even that was initially
masked: a standalone `float d = 12.5f - 4.25f;` (both literal constants)
gave the *correct* answer, because Clang's frontend constant-folds a
literal-literal subtraction at parse time, before `--hwfloat` or
`__subsf3_hw` ever enter the picture — so the very first "does subtract
work" test looked like a pass while testing nothing. The bug only
showed up once real *variables* were subtracted (`float x=12.5f,
y=4.25f; float r = x - y;` — the smallest possible repro, no reuse or
sequencing needed at all). Diagnosed by decisively ruling out every
other candidate first, each with its own direct probe: the IEEE↔hex-
float conversion functions (point 7) round-trip-verified correct in
isolation when called from ordinary compiled C; `FLDS`/`FAS`/`FSTS`
verified correct with large ("bulk") addresses; the `FSS` instruction's
own `FPAC1 -= FPAC0` semantics re-verified with a clean, assumption-free
harness (`FLDS 0,A / FLDS 1,B / FSS 0,1` → result `= B - A`, checked
both operand-value orderings); and finally the actual compiled call
site for `x - y` hand-traced address-by-address through `main`'s own
(fairly involved, large-frame, indirectly-addressed) generated code,
confirming the *caller* really does push `x` before `y` — which, given
the right-to-left convention above, means `x` (the first source
argument) lands in the *second* argument slot, not the first. Fixed by
swapping which parameter offset (`FP-8`/`FP-7` vs `FP-6`/`FP-5`) this
file reads into its own `__hwf_a_*`/`__hwf_b_*` scratch, in both
`__addsf3_hw` and `__subsf3_hw` (the fix is invisible for `__addsf3_hw`,
as expected, but was applied there too since it's the same genuine ABI
fact either way).

**7. IEEE-754 ↔ hex-float bridge — the format stays IEEE-754
everywhere else.** This target's `float` ABI, `printf`/`print_float`,
struct layout, and the existing software `__addsf3`/etc. all use
IEEE-754 single precision, unconditionally, unchanged by this feature.
Only `__addsf3_hw`/`__subsf3_hw`'s own bodies know the hardware's
native hex-float format exists: two new C functions,
`ieee754_to_hexfloat32`/`hexfloat32_to_ieee754` (`eclipse_rt.c`, non-
static so the hand-written assembly can call them by name — see that
file's own comment on why), convert at each function's entry/exit.
Pure integer bit manipulation (no floating point involved in the
conversion itself) — extract sign/exponent/mantissa, align the binary
mantissa to a 4-bit (hex-digit) boundary via `e = ceil((E+1)/4)`,
round-to-nearest at the alignment shift, renormalize on carry-out,
re-pack. Ported from a standalone Python implementation checked against
24 test values (bit-exact for every value with a clean hex-float
representation; ~1e-5 relative rounding difference for the couple of
values needing more precision than hex float's 4-bit exponent
granularity can represent exactly — an inherent "wobble" property of
the *format itself*, not a conversion bug, consistent with hex-float's
well-documented reduced effective precision versus IEEE binary). Two
implementation constraints already established elsewhere in this file's
soft-float section applied directly: variable-*amount* shifts on a
32-bit value lower to `SRL_PARTS`/`SHL_PARTS`, which this backend can't
select — used the existing `sf_shl`/`sf_shr` bit-at-a-time helpers
instead of a raw `<<`/`>>` with a runtime amount; and a raw 32-bit
comparison used directly in a branch condition crashes ISel — used the
existing `u32_eq`/`u32_lt`/`u32_ge` non-inlined-function-call helpers
for every 32-bit comparison, mirroring `sf_normalize`'s own
already-proven pattern exactly.

**8. Verification results.**

*Feature OFF (default, every existing invocation)* — required to be
byte-for-byte unchanged:
- `regress_hwstack.sh`: every example clean-HALTs with correct output,
  plus exactly the 4 known/expected "Step expired" cases (`isr_c_test`,
  `test_fps_add`, `test_fps_md`, `fps_dma_test`) — unchanged from
  baseline.
- Full c-testsuite: **197/220** — unchanged from the documented floor.
- `sanity_add.s` `diff` (point 4): confirms zero codegen difference
  anywhere outside the two libcall symbol names, for a program that
  doesn't even use `--hwfloat`.

*Feature ON* (`examples/test_hwfloat.c`, compiled with
`eclipse-cc --hwfloat`, run on real `eclipseemu`) — whole numbers,
negative operands, values crossing to negative, zero (including
`-5.0 - -5.0`), fractional values, and division-then-hardware-subtract
(`1/4 - 1/8`), plus the reused-variable regression case from point 6:

```
16.750000   (12.5 + 4.25)
8.250000    (12.5 - 4.25)
-1.500000   (-3.5 + 2.0)
-5.500000   (-3.5 - 2.0)
-899.000000 (100.0 - 999.0)
0.000000    (0.0 + 0.0)
0.000000    (-5.0 - -5.0)
0.125000    (0.0625 + 0.0625)
0.125000    (1/4 - 1/8, software divide + hardware subtract)
16.750000   (x + y, reused variables)
8.250000    (x - y, reused variables -- was -8.25 before the fix)
```

— all correct, and (compiled *without* `--hwfloat`, same source)
byte-identical output from the unmodified default software path.

**9. Known gap, pre-existing and unrelated — not fixed here.** While
building the reused-variable regression test, a genuine, *pre-existing*
bug surfaced: `static float ga = 12.5f; static float gb = 4.25f;`
(file-scope float globals, as opposed to locals) print `0.000000` for
both `ga+gb` and `ga-gb` — confirmed present with `--hwfloat` off too
(plain default software float), so this predates and is unrelated to
this entry's work. Left unfixed and out of scope: this entry's mandate
is float add/subtract via real hardware, opt-in and additive, with the
existing software path byte-for-byte preserved — a pre-existing
software-float bug is explicitly a "leave it exactly as it already
behaves" case, not something to fix incidentally. Flagged here as a
real, reproducible follow-up for whoever picks up global float support
next; not reproducible with function parameters or ordinary locals,
only file-scope `static`/global `float` variables specifically.

**Page-zero cost**: `__addsf3_hw`/`__subsf3_hw`'s own `EJSR`/`ISZ`/`DSZ`
call-site bookkeeping costs nothing beyond what any other function call
already costs (the shared `_SP`-equivalent hardware-stack-pointer
mechanism, not per-callee state) — their `__hwf_*` scratch is
deliberately bulk, not page-zero (point 6), specifically so this
feature doesn't compete for the shared 256-word budget at all when
enabled.

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
{ cat test.simh; echo 'dep PC 50'; echo 'step 100000'; echo 'e PC'; \
  echo 'quit'; } | eclipseemu
```

### 28. Quotient and remainder of the same operands, needed live simultaneously, silently corrupted each other (fixed) — a genuine hidden-clobber bug in `UDIVrr`/`UREMrr`, replaced with a combined `UDIVREMrr`

**Repro:**

```c
#include <stdio.h>
int main(void) {
    unsigned int ua = 65000, ub = 7;
    printf("%u %u\n", ua / ub, ua % ub);    /* should print "9285 5" */
    unsigned int a2 = 100, b2 = 7;
    printf("%u %u\n", a2 / b2, a2 % b2);    /* should print "14 2" */
    unsigned int a4 = 65000, b4 = 3;
    printf("%u %u\n", a4 / b4, a4 % b4);    /* should print "21666 2" */
    return 0;
}
```

Before this fix, real `eclipseemu` printed `9285 0`, `14 0`, `21666 0` —
the quotient was always right, the remainder always `0`. Computing
division and remainder of the same two operands *separately*, at
unrelated call sites not needing to be simultaneously live, already
worked correctly; the bug was specifically about needing both results
of the *same* division live at the same time.

**Root cause.** `UDIVrr`/`UREMrr`'s post-RA expansion
(`EclipseInstrInfo.cpp`'s `expandPostRAPseudo`) drives the real
zero-operand hardware `DIV` instruction by saving AC2 (this backend's
reserved frame pointer) to the hardware stack, moving `$rhs`/`$lhs`
into AC2/AC1, unconditionally zeroing AC0 (`SUB 0,0,0` — primes DIV's
dividend high word), running `DIV`, copying out *one* of AC0
(remainder) or AC1 (quotient) into `$dst`, then restoring AC2. AC0 and
AC1 both get unconditionally destroyed by this sequence, but
`EclipseInstrInfo.td` only ever declared AC2 as an implicit clobber
(`Defs = [AC2]`) — a comment already on that `.td` line, from an
earlier session, records that declaring AC0/AC1 too was tried once and
broke register allocation outright ("ran out of registers"), because
with only two allocatable registers on this target (AC0/AC1 — AC2/AC3
are permanently reserved), `$dst`/`$lhs`/`$rhs` already exhaust the
entire pool on their own; adding AC0/AC1 as *additional* implicit Defs
double-counts register demand the allocator can never satisfy.

So the register allocator, with no way to know AC0/AC1 get clobbered
beyond `$dst`, could (and did, for this repro) keep one `UDIVrr`
pseudo's already-computed quotient result live in AC0 or AC1 across a
subsequent `UREMrr` pseudo computing the remainder of the same two
operands — and that second pseudo's own operand-marshalling sequence
(zero AC0, move `$lhs` into AC1) silently overwrote the first pseudo's
result before it was ever read. Two *separate* pseudo instructions,
each individually respecting the declared 2-register contract, still
collided with each other's *undeclared* clobber.

**Why the obvious fix doesn't work.** Just adding `AC0, AC1` back to
`UDIVrr`/`UREMrr`'s `Defs` would correctly model the clobber, but is
exactly the change a previous session already tried and rejected (see
the `.td` comment, and `EclipseISelLowering.cpp`'s `LowerShift`
comment on `UDIVrr`) — with only two allocatable registers, declaring
that `UDIVrr` clobbers *both* AC0 and AC1 in addition to writing
`$dst` (itself one of AC0/AC1) leaves no register anywhere for the
allocator to keep *any other value* live across the instruction,
breaking allocation for any function with more concurrent live values
than that.

**The actual fix: stop hiding the clobber by declaring it truthfully.**
The real hardware `DIV` instruction already produces *both* quotient
(AC1) and remainder (AC0) from a single execution, regardless of which
one the IR actually asked for — `UDIVrr` and `UREMrr` were each
independently running that same full sequence and simply discarding
whichever half they didn't need, then rerunning the *entire* sequence
from scratch if the other half was also needed. Replaced both with a
single new pseudo, `UDIVREMrr` (`EclipseInstrInfo.td`), with **two**
declared outputs — `(outs GPR:$quot, GPR:$rem)` — that runs the save-
AC2/marshal-operands/zero-AC0/`DIV`/restore-AC2 sequence exactly once
and extracts *both* real results. Since `$quot`/`$rem` are drawn from
the very same 2-register GPR pool as `$lhs`/`$rhs` (which are dead by
the time `$quot`/`$rem` are born, exactly as `$dst` already relied on
for the single-output pseudos), there is no undeclared clobber left to
hide: the register allocator now sees the true, complete register
demand of a `DIV` up front, and correctly refuses to keep anything else
live across it — which is the actual hardware constraint, truthfully
modeled, not the allocation-breaking over-declaration the earlier
attempt made.

`expandPostRAPseudo`'s `UDIVREMrr` case: `$quot`/`$rem` are necessarily
some permutation of exactly `{AC0, AC1}`. If the allocator happened to
assign them `quot=AC1, rem=AC0` — exactly where `DIV` naturally leaves
its two results — nothing further is needed. Otherwise, AC2 (dead at
that point: its saved copy of `$rhs` was already consumed by `DIV`, and
it is not restored to the caller's frame pointer until the very end of
the expansion) serves as scratch to swap AC0/AC1 into the assigned
registers.

**Wiring the combine in.** `ISD::UDIV`/`ISD::UREM` are now `Expand`
instead of the default `Legal`, and `ISD::UDIVREM` is `Custom`
(`LowerUDIVREM`, built directly as a machine node via
`DAG.getMachineNode` — the same technique `LowerSTACKSAVE`/
`LowerDYNAMIC_STACKALLOC` already use for their own multi-result
pseudos, `STACKSAVE_PSEUDO`/`VLAALLOC` — rather than a TableGen `Pat`,
since this codebase has no existing precedent for matching a 2-result
generic SDNode straight to a 2-out instruction via `Pat`). This routes
*every* `udiv`/`urem` — standalone or paired — through `UDIVREMrr`.
Confirmed this costs nothing extra even for a standalone division (it
is the same one hardware `DIV` either way): generated assembly for the
3-division repro above shows exactly 3 `DIV` instructions, not 6 as it
would from two independent `UDIVrr`+`UREMrr` calls.

A genuine `udiv`+`urem` pair of the same operands collapses onto the
same `UDIVREMrr` regardless of *how* the pairing is discovered — no
fragile combine-ordering assumption needed: `DAGCombiner::useDivRem()`
takes the earlier opportunity to fuse an explicit matching pair into
one `ISD::UDIVREM` node before legalization ever runs; failing that,
`TargetLowering::ExpandNode` (for a lone `udiv`) and
`TargetLowering::expandREM` (for a lone `urem`) each independently
convert their own node to `DAG.getNode(ISD::UDIVREM, dl, {VT,VT}, LHS,
RHS)` during legalization — and since `SelectionDAG::getNode` CSEs on
opcode+VTList+operands, two independent calls with identical operands
produce the very same node either way. Signed `sdiv`/`srem`
(`LowerSDIVREM`, decomposed into `abs`+`udiv`/`urem`+conditional
negate) benefit for free from the same CSE, since a single call site
needing both `a/b` and `a%b` for the same signed `a,b` calls
`LowerSDIVREM` twice with identical `LHS`/`RHS`, and the two
`AbsVal(LHS)`/`AbsVal(RHS)` subgraphs it independently builds are
themselves CSE'd into the same nodes. `LowerShift`'s `SRA` path (`x >>s
n` for non-power-of-2-shift-adjacent code) turns out to have had the
identical latent bug all along — it directly builds `ISD::SDIV` and
`ISD::SREM` of the same `X`/`Divisor` in one function, a same-operands-
needed-simultaneously case never previously tested in isolation.

`UDIVrr` is kept, pattern-less, for `EclipseHalve`'s own dedicated
`Pat` (the internal GEP-offset-scaling divide-by-2 that never needs a
remainder). `UREMrr` is kept defined and still handled by
`expandPostRAPseudo`, but is no longer referenced by any pattern —
left in place rather than deleted in case a remainder-only pseudo is
useful again later.

**Verified on real `eclipseemu`** (all previously wrong, now correct):

- The repro above: `9285 5` / `14 2` / `21666 2`.
- Signed division/remainder of shared operands (`int a = -17, b = 5;
  printf("%d %d", a/b, a%b);` and permutations of operand signs):
  `-3 -2`, `-3 2`, `14 -2`.
- Div+rem of the same operands passed as function parameters, both
  unsigned and signed.
- Div+rem of the same operands read from array elements and from
  global variables.
- 32-bit `long`/`unsigned long` div+rem of shared operands
  (`__udivsi3`/`__umodsi3`/`__divsi3`/`__modsi3` in `eclipse_rt.c`) —
  these go through ordinary libcalls with the normal call/return
  convention, not this pseudo, so were never actually exposed to this
  bug; re-verified anyway (`613566742 1`, `-176366841 -3`) per the
  task's own instruction to check the 32-bit path.
- `LowerShift`'s `SRA` case (`int e = -12345; e >> 5;` and further
  cases): `-13`, `-1`, `-386` — all correct.

**Full regression, byte-identical to baseline**: `regress_hwstack.sh`'s
15 examples — every `HALT` case's printed output textually unchanged
from the pre-fix baseline (final PC values differ, as expected, since
code layout shifted), and exactly the same 4 pre-existing `Step
expired` cases as always (`isr_c_test`, `test_fps_add`, `test_fps_md`,
`fps_dma_test` — unrelated FPU/device-polling limitations from earlier
entries).

c-testsuite: 197/220, re-measured immediately before this change and
again after — the two full result files (`run_all_tests.sh`'s
per-test PASS/MISMATCH/COMPILE_FAIL/TIMEOUT output, not just the
197 count) are byte-identical (`diff` reports no differences at all).
The 23 non-passing test IDs are unchanged: `00040`, `00041` (TIMEOUT),
`00104`, `00174`, `00187`, `00189`, `00204`, `00220` (COMPILE_FAIL),
`00157`, `00163`, `00166`, `00168`, `00175`, `00178`, `00179`, `00180`,
`00195`, `00203`, `00205`, `00212`, `00217`, `00218` (MISMATCH),
`00216` (TIMEOUT) — no regressions, no new passes (c-testsuite's own
test set doesn't happen to exercise this specific shared-operands-
live-simultaneously shape).

**Page-zero cost**: none — `UDIVREMrr` reuses the exact same
instruction sequence `UDIVrr`/`UREMrr` already emitted (same
`ISZABS`/`STABSI`/`MOVrr`/`SUBrr`/`DIV`/`LDABSI`/`DSZABS` building
blocks, same page-zero footprint), just with both real outputs
declared instead of one hidden.

### 29. print_dg_float(hi, lo): print a real-hardware DG hex-float value given as two 16-bit words, decoded DIRECTLY (no IEEE-754 conversion)

A user has separate hardware that returns 32-bit DG/IBM-style hex-format
floats as a pair of 16-bit words (the same number format entry #27's
`--hwfloat` real Eclipse FAD/FAS/etc. instructions use -- see the main
backend README.md's Floating Point Instructions section), calling this
from debug-heavy code (so call-site instruction cost genuinely matters,
not just a one-off diagnostic).

**First version** (superseded, described here only because the design
choice is worth recording): a thin wrapper reusing entry #27's already-
verified `hexfloat32_to_ieee754` bridge plus the existing `print_float`
-- pack the two words, convert to this target's IEEE-754 ABI
representation, print normally. Correct and simple, but pays for a real
conversion (normalize, re-bias, round) on every call that a direct
decoder doesn't need to.

**Final version: decode the hex-float bits directly, no IEEE-754
step at all.** Both formats reduce to the same shape once you strip the
format-specific packaging -- a plain integer significand times a power
of two (`value = mant * 2^shift`, with `shift = 4*(exponent_field-64) -
24` for a hex float; no separate "hidden bit" trick needed the way
IEEE-754 has one, since a hex-float mantissa's own leading nonzero hex
digit already IS what IEEE-754 keeps implicit). `print_float`'s existing
`print_float_frac` (the decimal-digit-extraction loop) only ever
consumes `pf_frac_bits`/`pf_fbits_n` -- genuinely agnostic to which
float format they came from -- so it's reused completely unchanged. The
only new code is deriving (integer part, `pf_frac_bits`, `pf_fbits_n`)
from the hex-float's own fields directly, the equivalent of what
`print_float_extract`/`__fixsfsi` do for IEEE-754's fields (`__fixsfsi`
itself can't be reused -- it's IEEE-754-bit-pattern-specific).

```c
void print_dg_float(unsigned int hi, unsigned int lo) {
  u32 bits = ((u32)hi << 16) | (u32)(lo & 0xFFFFUL);
  if (u32_and_nz(bits, 0x80000000UL)) {
    putchar('-');
  }
  u32 mant = bits & 0x00FFFFFFUL;
  pf_frac_bits = 0;
  pf_fbits_n = 0;
  if (u32_eq(mant, 0)) {
    print_uint32(0);
    putchar('.');
    print_float_frac();
    return;
  }
  int exp_field = (int)((bits >> 24) & 0x7FUL);
  int shift = 4 * (exp_field - 64) - 24;

  u32 uip;
  if (shift >= 0) {
    uip = sf_shl(mant, shift);
  } else {
    int nshift = -shift;
    uip = sf_shr(mant, nshift);
    pf_fbits_n = nshift;
    pf_frac_bits = mant & (sf_shl(1UL, nshift) - 1UL);
  }
  print_uint32(uip);
  putchar('.');
  print_float_frac();
}
```

`hi` holds bits 31-16 (sign, 7-bit excess-64 exponent, and the top two
hex digits of the mantissa); `lo` holds bits 15-0 (the remaining four
hex digits) -- swap the two arguments at the call site if a particular
source hands them back the other way around.

**On the wider shift range, and why no new bounds/saturation logic was
needed**: `sf_shl`/`sf_shr` (used here exactly as `print_float_extract`
already uses them) shift one bit at a time in a loop and are safe for
*any* shift amount, not just IEEE-754's usual range -- confirmed by
reading their own implementation before relying on this, not assumed.
Hex float's wider exponent field means `shift` can run roughly ±280 here
versus IEEE-754's ~±150, correspondingly more loop iterations for an
extreme-magnitude value, but no new special-casing was needed: an
oversized rightward shift converges to 0 (both for the integer part and,
later, every decimal digit `print_float_frac` extracts) exactly like a
genuinely tiny value should print as all zeros at 6-decimal-place
precision, and an oversized leftward shift saturates the same way a
very large IEEE-754 magnitude already does through `__fixsfsi`'s own
existing behavior.

**Verified**: the same seven hand-picked DG hex-float bit patterns as
the first version (1.0, 2.0, 4.0, 8.0, 0.5, -1.0, 3.0 -- each
independently computed and cross-checked against the format's own
definition, `mantissa_int * 2^(4*(exponent_field-64)-24)`, with a
standalone Python script before use, not just eyeballed) still print
correctly after the rewrite, byte-for-byte identical output to the
first version. Additionally verified: `+0.0`/`-0.0` (prints
`0.000000`/`-0.000000`, sign preserved), and two values landing in the
`shift >= 0` pure-integer branch specifically (16.0, 256.0) plus one
small fraction (0.0625) -- all correct on real `eclipseemu`. Full
existing regression suite (`regress_hwstack.sh`) re-run and confirmed
byte-identical to baseline after the rewrite.

**Measured cost**: a single call, isolated (binary-searched exact
`eclipseemu` instruction count to `HALT`) -- 40,338 instructions via the
first (conversion-based) version, 36,723 via the direct version: roughly
3,600 instructions saved per call (~9%), not the order-of-magnitude
difference that might be guessed from "skips an entire conversion
function" -- both versions share the same underlying fixed cost
(`print_uint32` for the integer part, `print_float_frac`'s decimal
loop), so the saving is bounded to what `hexfloat32_to_ieee754`'s own
normalize/re-bias/round work actually cost, not the whole call. Real
and worth it for debug-heavy call sites, as confirmed directly rather
than assumed from an "obviously faster" intuition.

`eclipse-toolchain/rt/eclipse_rt.c` and
`eclipse-package/eclipse-toolchain/rt/eclipse_rt.c` (and both copies'
`rt/include/stdio.h`) kept byte-identical, per this project's standing
convention. `hexfloat32_to_ieee754`/`ieee754_to_hexfloat32` themselves
are untouched -- still exactly what entry #27's `--hwfloat` machinery
uses; only this file's own `print_dg_float` stopped routing through
them.

### 30. DG hardware-accelerated float becomes the DEFAULT (`--ieee` opts out), plus a real global-float-initializer bug fixed — and why a true native hex-float ABI was scoped back out

The task going in: make native DG hex-float the toolchain's *default*
for `float`/`double` — constant emission, ABI representation, arithmetic,
comparison, int↔float conversion, and printing all genuinely hex-float,
end to end — with today's IEEE-754 behavior (including the narrower
existing `--hwfloat`) demoted to an explicit opt-in, and re-verifying
`FMS`/`FDS`/`FLAS`/`FFAS`/`FCMP` fresh rather than trusting entry #27's
"unreliable" verdict. What actually shipped is narrower than that, for a
concrete, verified reason found partway through (see part 3) — this
entry is written to be honest about exactly where the line landed and
why, not to overstate it.

**1. Fresh re-verification of `FMS`/`FDS`/`FLAS`/`FFAS`/`FCMP` on
`eclipseemu`.** Built a clean, assumption-free hand-assembled harness
(`org 050`, `FLDS`-loaded known-good hex-float operands — `2.0 =
0x41200000`, `3.0 = 0x41300000`, `4.0 = 0x41400000`, same constants
entry #27 already verified — single-stepped, registers inspected via `e
FPAC0`-`e FPAC3`/`e AC0`-`e AC3`), independent of the original entry #27
probe.

- **`FMS`/`FDS` (multiply/divide): reconfirmed genuinely broken.**
  `FMS 0,1` (2.0×3.0) and the reversed `FMS 1,0` (3.0×2.0) both leave the
  *destination* `FPAC` at exactly zero while the source operand's `FPAC`
  is untouched — confirmed with both operand orderings. Same result for
  `FDS` (4.0÷2.0, both orderings). This matches entry #27's original
  finding exactly, now independently re-derived rather than assumed.
  Not used.
- **`FLAS`/`FFAS` (int↔float conversion): entry #27's "operand-encoding-
  dependent, inconsistent" verdict was a probing mistake, not a real
  hardware limitation.** First reproduced the original confusion
  directly: `LDA 1,int5` (`AC1=5`) followed by `FLAS 0,1` produced
  nothing in `FPAC0` *or* `FPAC1`. But `FLAS 1,1` (loading the *same*
  register number that's mentioned in the second operand slot) worked —
  `AC1`'s `5` correctly became `FPAC1`'s `5.0`. That pattern (only
  "matching-numbered" operands worked) is what looked like
  "inconsistent" the first time around. Redone with the actual right
  hypothesis — `FLAS srcAC,dstFPAC` (first operand is the source AC,
  *not* tied to the destination FPAC number at all) — and it is
  completely reliable: `LDA 1,int5; FLAS 1,0` → `FPAC0 = 5.0`; `LDA
  0,int5; FLAS 0,1` → `FPAC1 = 5.0`; `LDA 2,int5; FLAS 2,0` → `FPAC0 =
  5.0`; `LDA 3,int7; FLAS 3,1` → `FPAC1 = 7.0` — four different
  register-number combinations, all correct. (One real quirk, harmless:
  `FLAS`'s output mantissa isn't always in canonical normalized form —
  e.g. `5` came back as hex-float bits decoding to hi-word `0x4205`
  (exponent one too high, leading mantissa nibble `0`) instead of the
  canonical `0x4150`/`0x4205`-equivalent form a real `FAS`/`FSS` result
  would use — but both encode the exact same value, `5.0`, and
  `hexfloat32_to_ieee754`'s own normalize loop already tolerates any
  number of leading zero bits, not just the ≤3 a canonical hex-float
  needs, so this wouldn't break anything that used it.) `FFAS` likewise
  reliable once its own (different, and this time already-correctly-used
  in the original probe) operand order — `FFAS dstAC,srcFPAC` — is
  understood: `FFAS 0,0`/`FFAS 0,1`/`FFAS 1,0`/`FFAS 2,0`, `FPAC` holding
  `3.0` each time, all correctly produced `3` in the named destination
  AC. Not used regardless (see part 3) — both instructions only convert
  a single 16-bit AC, and this target's `SINTTOFP_I32_F32`/
  `FPTOSINT_F32_I32` libcalls need a genuine 32-bit `long`; combining two
  16-bit halves into one 32-bit hardware-assisted conversion would need
  new, unverified logic (and, given `FMS`/`FDS`'s state, almost certainly
  *not* a hardware multiply to do the high-word scaling) — flagged as a
  real, well-scoped follow-up, not forced into this entry.
- **`FCMP`**: executes without error and leaves both `FPAC` operands
  unmodified (checked `2.0` vs `3.0`, `3.0` vs `2.0`, `2.0` vs `2.0`).
  Deliberately not characterized further (the `FSEQ`/`FSGT`/... skip-
  instruction family was not probed) — see part 3: comparison already
  has a proven, format-agnostic software path this entry doesn't need to
  replace.

**2. Why a true native, end-to-end DG hex-float ABI is not reachable
without patching common LLVM infrastructure.** This target's `float` has
no register class and no Custom lowering (entry #27 point 4) — every
float value is generically "softened" by LLVM's own target-independent
type legalizer before this backend's code ever sees it. For a *constant*
specifically, that softening is
`DAGTypeLegalizer::SoftenFloatRes_ConstantFP`
(`llvm/lib/CodeGen/SelectionDAG/LegalizeFloatTypes.cpp`), and reading it
directly (not assumed from memory) confirms it unconditionally does
`CN->getValueAPF().bitcastToAPInt()` — the plain IEEE-754 bit pattern —
with no target-overridable hook of any kind. That function runs for
*every* local float constant/temporary materialized inside a function
body (e.g. `float x = 12.5f;`), regardless of any `SubtargetFeature`.
There is no lever this backend can pull to make a local float constant
carry a different (hex-float) bit pattern short of patching that shared,
target-agnostic function — real surgery affecting every LLVM target that
uses generic float softening, explicitly the same class of out-of-scope
change as touching `APFloat` itself (which the original task already
ruled out for the same reason).

Consequence: if `float` values flowing through registers/stack/locals
stayed IEEE-754 (unavoidable) while global storage were converted to
genuine hex-float bits, the two would silently disagree the moment a
global and a local met in the same expression (`g + x`) — a real,
serious correctness landmine, not a cosmetic gap. So this entry does
*not* attempt a native hex-float ABI. The design instead keeps the ABI
IEEE-754 *everywhere*, exactly as before, and narrows "DG-native by
default" down to what's actually safely reachable:

- **`ADD_F32`/`SUB_F32` default to real hardware** (`FAS`/`FSS`, via the
  exact same, already-verified `__addsf3_hw`/`__subsf3_hw` bridge entry
  #27 built — reused verbatim, not reimplemented) — same
  convert-in/compute/convert-out shape as today's `--hwfloat`, just made
  the default instead of opt-in. `MUL_F32`/`DIV_F32`/int↔float conversion
  stay on their original software implementations unconditionally either
  way (part 1's `FMS`/`FDS` findings, and `FLAS`/`FFAS`'s 16-bit-only
  limitation).
- **A real, independent, pre-existing bug fixed for the new default**:
  `EclipseAsmPrinter::emitGlobalVariable` had no `ConstantFP` case at all
  before this entry — a file-scope/`static` `float`/`double` global's
  initializer fell through to the generic "unsupported leaf constant"
  fallback and silently materialized as a single zero word (entry #27
  point 9's "known gap, pre-existing and unrelated"). Fixed for the new
  default only: emits the constant's real IEEE-754 bit pattern (*not*
  hex-float, per the ABI decision above) as two words, high-then-low,
  using the exact same `_word1`-suffix/most-significant-first convention
  the existing >16-bit `ConstantInt` case already establishes (so
  existing "access word N of a bulk global" addressing finds it with no
  further backend support needed). Same handling added to the struct-
  field-flattening path (`flattenConstant`) for a `float`/`double` struct
  field. Deliberately gated OFF (falls through to the original
  "unsupported → 0" behavior) when `+ieee` is set — see part 4.
- **Comparison libcalls need no change at all, in either mode.**
  `eclipse_rt.c`'s `sf_cmp` (backing `__eqsf2`/`__ltsf2`/etc.) works by
  masking off the sign bit and comparing the remaining bits as a plain
  unsigned integer — it never decomposes into separate exponent/mantissa
  fields with hardcoded masks. That's format-agnostic: it would have
  worked unchanged even for a genuine hex-float ABI (same "exponent then
  mantissa, MSB-first" packing property IEEE-754's own bit-comparison
  trick relies on), so with the IEEE-754-everywhere design actually
  shipped here, it's simply unaffected. Not touched.

**3. A genuine, pre-existing latent bug found and fixed along the way:
member declaration order silently reset `HasHWFloat`/`HasIEEEFloat` back
to `false` after `ParseSubtargetFeatures` had already set them.**
`EclipseSubtarget.h` declared `HasHWFloat` (and, until this was found,
the new `HasIEEEFloat`) *after* `InstrInfo`/`FrameLowering`/`TLInfo`/
`TSInfo`. C++ initializes data members in declaration order regardless
of the constructor's own initializer-list order, and `HasHWFloat`/
`HasIEEEFloat` have no explicit mention in `EclipseSubtarget`'s
constructor initializer list — so even though `ParseSubtargetFeatures`
(run as a side effect of `InstrInfo`'s own initializer, via
`initializeSubtargetDependencies` — see that method's comment) correctly
set them earlier in the *same* construction sequence, the compiler still
applies their in-class default member initializer (`= false`) when
construction reaches *their own* declaration position — which, being
declared after `InstrInfo` et al., happens *later*, silently
overwriting the correct value right back to `false`.

Found empirically, not just reasoned about: an instance-address-tagged
`errs()` print in both `EclipseSubtarget`'s constructor body and (the
new) `EclipseAsmPrinter::emitGlobalVariable` showed the identical `this`
pointer but `HasIEEEFloat` reading `false` at both points — while the
*generated code* for that exact same build correctly used the software
`__addsf3` under `+ieee` (`EclipseTargetLowering`'s constructor reads
`useHardwareFloat()` *during* `EclipseSubtarget` construction, i.e.
before the reset). That contradiction (same object, same field, correct
transient value seen mid-construction vs. `false` seen immediately after
and forever after) was the tell. This is a real, pre-existing bug that
has been present for `HasHWFloat` since entry #27 — it was invisible
until now purely because every existing reader of `hasHWFloat()`
happened to run *during* construction (`EclipseISelLowering.cpp`'s
constructor, `EclipseSubtarget::initLibcallLoweringInfo`'s own
`ADD_F32`/`SUB_F32` entries feeding `DAG.getLibcalls()`, which per that
method's own comment is consulted only by the comparison-libcall-
selection path, not real `ADD_F32` codegen) rather than after it. This
entry's `emitGlobalVariable` fix is the first-ever genuinely
*post-construction* consumer, which is what exposed it. Fixed by moving
`HasHWFloat`/`HasIEEEFloat`'s declarations to *before*
`InstrInfo`/`FrameLowering`/`TLInfo`/`TSInfo` in the class body — no
change to `initializeSubtargetDependencies`/`ParseSubtargetFeatures`
themselves, which were already correct.

**4. Design: `FeatureIEEEFloat` (`Eclipse.td`, opt-out, off by
default).** `useHardwareFloat() = !HasIEEEFloat || HasHWFloat` (new
method on `EclipseSubtarget`, single source of truth consulted by both
`EclipseSubtarget::initLibcallLoweringInfo` and
`EclipseISelLowering.cpp`'s constructor — mirroring how `HasHWFloat`
alone was already consulted in two places, see that class's own
comment). No flags at all → `HasIEEEFloat=false`, `HasHWFloat=false` →
`useHardwareFloat()=true` → the new default (hardware add/subtract,
fixed globals). `+ieee` → `HasIEEEFloat=true` → `useHardwareFloat()`
false unless `+hwfloat` is *also* set → this target's entire original,
pre-entry-#30 default (software arithmetic, buggy globals preserved).
`+hwfloat` alone (direct `-mattr=`, not through `eclipse-cc`) is
equivalent to the new default (`useHardwareFloat()=true` either way) —
harmless, just redundant. `eclipse-cc`'s own `--hwfloat` flag sets
*both* `+hwfloat` and `+ieee` together (its existing header comment
explains why), so `eclipse-cc --hwfloat` alone continues to reproduce
its own pre-entry-#30 meaning exactly: IEEE-754 ABI/storage, hardware
add/subtract only, global-initializer bug still present. New
`eclipse-cc --ieee` flag, and a thin `eclipse-cc-ieee` wrapper script
(same directory) for tooling that wants a single `CC_SCRIPT`-shaped
invocation (`run_all_tests.sh`'s own convention). `eclipse-cc` also had
to stop gating the textual append of `rt/eclipse_hwfloat.s` on
`$hwfloat` alone — it now has to happen whenever
`useHardwareFloat()`-equivalent logic (`ieee=0` OR `hwfloat=1`) is true,
since the new default needs `__addsf3_hw`/`__subsf3_hw` linked in too;
got this wrong on the first pass (dgasm correctly reported "Undefined
symbol: `__addsf3_hw`" for the new default before this was fixed —
exactly the kind of loud, unambiguous failure this project's own
"dgasm's exit code isn't trustworthy, but undefined-symbol errors are"
observation already covers).

**5. Verification.**

*`--ieee` (the real regression safety net now — must reproduce every
pre-entry-#30 build byte-for-byte):*
- `regress_hwstack.sh`'s full suite (15 examples), run with `--ieee`
  added to every invocation: every example clean-HALTs with correct
  output, plus exactly the 4 known/expected "Step expired" cases
  (`isr_c_test`, `test_fps_add`, `test_fps_md`, `fps_dma_test`) —
  unchanged from baseline.
- Full c-testsuite (`eclipse-cc-ieee` as the `CC_SCRIPT`): **197/220**,
  and the exact same 220-test pass/fail *set* as the documented baseline
  (diffed test-by-test, not just the aggregate count).
- `examples/test_hwfloat.c` compiled with `--ieee --hwfloat`: reproduces
  entry #27's own 11 expected values exactly (`16.750000`, `8.250000`,
  `-1.500000`, `-5.500000`, `-899.000000`, `0.000000`, `0.000000`,
  `0.125000`, `0.125000`, `16.750000`, `8.250000`).
- A dedicated new test (`static`/global `float` and `double`
  initializers, arithmetic, comparisons — see below) compiled with
  `--ieee`: every global reads back as `0.000000`, and every value
  derived from one is consistent with that (`0+0=0.000000`,
  `0-0=-0.000000`, comparisons against a `0`-valued global all `0`/false)
  — the pre-existing bug reproduced exactly, confirming `+ieee` really
  does take the untouched original code path, not a coincidentally-
  similar new one.

*Default (no flags at all):*
- Same `regress_hwstack.sh` suite, no flags: identical structure (11
  clean HALTs, the same 4 "Step expired" cases) *and* identical output
  values to the `--ieee` run, line for line — diffed directly, zero
  differences anywhere in the suite.
- Full c-testsuite, no flags: **197/220**, and — diffed test-by-test
  against the `--ieee` run — the *exact same* pass/fail set, not just
  the same count. Zero observed regressions, and (perhaps notable) zero
  observed hex-float-"wobble"-driven output differences either — this
  particular suite doesn't happen to exercise a case where hardware
  add/subtract's extra IEEE↔hex-float rounding step changes a printed
  digit.
- `examples/test_hwfloat.c`, no flags: identical output to the `--ieee
  --hwfloat` run above, value for value (expected — same
  `__addsf3_hw`/`__subsf3_hw` bridge either way).
- The dedicated new test, no flags: `static float g_pos = 12.5f;
  g_neg = -7.25f; g_frac = 0.0625f; g_zero = 0.0f; static double g_dbl =
  100.75; g_big = 12345.0f;`, printed directly, added/subtracted
  together, compared with `>`/`==`, and mixed with a local — every value
  correct and matching independently-computed (small standalone Python
  check, not hand-typed) expected results: `12.500000`, `-7.250000`,
  `0.062500`, `0.000000`, `100.750000`, `12345.000000`, `5.250000`
  (`g_pos+g_neg`), `19.750000` (`g_pos-g_neg`), `1`/`0`/`1`
  (`g_pos>g_neg`/`g_neg>g_pos`/`g_pos==12.5f`), `15.000000`
  (`g_pos+local`), `-10.000000` (`local-g_pos`) — confirming both the
  global-initializer fix and hardware add/subtract are correct together,
  including a `double` global (this target aliases `double` to the same
  32-bit `float` representation) and a global mixed with a local in the
  same expression.

**6. Known gap, honestly scoped, not forced.** `MUL_F32`/`DIV_F32` and
int↔float conversion stay on their original software implementations in
*every* mode — no hardware acceleration for those, by design (part 1's
findings). A genuinely native, end-to-end DG hex-float ABI (constants,
storage, and every arithmetic op all natively hex-float, eliminating the
IEEE↔hex-float conversion cost even for add/subtract) remains
infeasible without patching common LLVM infrastructure (part 2) — not
attempted, and not expected to become attempted without that
infrastructure change happening first. Extending `FLAS`/`FFAS`'s now-
confirmed-reliable single-16-bit-AC conversion into this target's actual
32-bit `SINTTOFP_I32_F32`/`FPTOSINT_F32_I32` libcalls is a real,
bounded, well-scoped follow-up opportunity this entry did not pursue.

`eclipse-cc` (new `--ieee` flag, `--hwfloat` now implies it, the
`eclipse_hwfloat.s`-inclusion condition fix) and the new `eclipse-cc-
ieee` wrapper kept byte-identical between
`eclipse-toolchain/eclipse-cc`(`-ieee`) and
`eclipse-package/eclipse-toolchain/eclipse-cc`(`-ieee`), per this
project's standing convention. `eclipse_rt.c`/`eclipse_hwfloat.s`
themselves are untouched by this entry (confirmed identical between both
copies) — every change here lives in the LLVM backend
(`Eclipse.td`/`EclipseSubtarget.{h,cpp}`/`EclipseISelLowering.cpp`/
`EclipseAsmPrinter.cpp`/`EclipseTargetMachine.h`) and in `eclipse-cc`
itself.
