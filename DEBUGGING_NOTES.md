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
{ cat test.simh; echo 'dep PC 50'; echo 'step 100000'; echo 'e PC'; \
  echo 'quit'; } | eclipseemu
```
