# Soft-float (`float`) support — implementation notes

## What works

IEEE-754 single-precision (`float`) arithmetic, comparisons, and
int↔float conversions are implemented and verified correct on
`eclipseemu`:

- `+`, `-`, `*`, `/` (`__addsf3`/`__subsf3`/`__mulsf3`/`__divsf3`)
- `==`, `!=`, `<`, `<=`, `>`, `>=` (all six comparison libcalls)
- `(float)int`, `(float)unsigned`, `(int)float`, `(unsigned)float`

See `examples/test_float*.c` for the verified test programs and their
expected output.

`double` (f64) is **not** implemented — it would need genuine 64-bit
integer support this backend doesn't have (see the main backend
`README.md`'s "Known limitations").

## Known limit: the shared page-zero budget

This target's `LDA`/`STA` instructions only have an 8-bit displacement
(page-zero, words 0–255), and every global, constant-pool entry, call
slot, and address slot in the *entire linked program* competes for that
same 256-word budget (see `reorder_asm.py`'s header comment). Combining
**many** substantial float operations in one program — e.g. all four
arithmetic ops plus all six comparisons at once — can exceed that budget
even though each operation is individually correct. This is a real
capacity limit of the actual hardware, not a bug. `eclipse-cc` /
`eclipse-compile.sh` only pay the (real, but avoidable) cost of
protecting soft-float symbols from dead-code-elimination for the
specific functions your program actually calls — see the comment in
either script for how.

## Bugs found and fixed along the way

None of these were specific to soft-float in the sense of "float math is
special" — they were all genuine, previously-undiscovered defects in how
this backend handles ordinary 32-bit (`unsigned long`/`long`) values,
multi-word function returns, and a couple of SelectionDAG-level
generalities. Soft-float just happened to be the first code to exercise
them hard enough to surface.

1. **Missing libcall wiring, at two separate levels.** LLVM's generic
   "soften float ops into libcalls" legalizer needs to be told which
   concrete symbol name implements each abstract libcall
   (`RTLIB::ADD_F32` → `__addsf3`, etc.) — but this has to be registered
   in *two* independent places: `EclipseTargetLowering`'s own copy
   (`EclipseISelLowering.cpp`'s constructor) and a separate, module-level
   copy (`EclipseSubtarget::initLibcallLoweringInfo`) that a different
   part of legalization (`SelectionDAG::getLibcalls()`) consults
   independently. Missing either one crashes `llc` ("unsupported library
   call operation") the first time that specific code path runs.

2. **Wrong `RTLIB` family for the comparison libcalls.** LLVM has two
   distinct conventions for float-compare libcalls: a "boolean" family
   (result tested with `!= 0`) and a "three-way" family (`-1`/`0`/`1`,
   tested with a predicate-specific condition code — the libgcc/
   compiler-rt convention this runtime's `sf_cmp()` actually implements).
   The six comparison functions were originally registered under the
   boolean family, which only coincidentally produced correct results
   for `!=` and the "true" cases of `<`/`>` — every other case (`==`,
   `>=`, `<=`, and the "false" cases) silently got the wrong condition
   code applied to the returned value.

3. **No 32-bit (2-word) return value support at all.** The calling
   convention only ever returned a single word in AC0; anything wider
   just `report_fatal_error`'d. Every soft-float arithmetic function
   returns a 32-bit `float`, so this was a hard blocker. Fixed by
   extending the convention to a proper AC0:AC1 register pair for 2-word
   returns — both registers are provably free at exactly that point (a
   function's return sequence), so no spill/reload machinery was needed.
   This is a genuinely general capability now, not float-specific.

4. **A DAG-combine bug affecting any sufficiently wide arithmetic.** A
   pre-existing combine (fixing an *older*, unrelated bug: runtime
   array-index scaling into indirectly-addressed globals) was
   over-matching a completely different, unrelated pattern: the address
   computation for the *second word* of any multi-word (32-bit) value —
   global **or local** — that needs indirect addressing. It was halving
   that address's already-word-granular offset (meant for byte-to-word
   conversion in the original case), silently corrupting which word got
   accessed. Two fixes were needed: excluding compile-time-constant
   offsets from the halving (the original case only ever needs it for
   *runtime*-computed offsets), and recognizing a bare stack `FrameIndex`
   base as needing the same exclusion as a global base.

5. **Multi-word global initializers silently truncated to one word.**
   `EclipseAsmPrinter::emitGlobalVariable` emitted a scalar initializer
   wider than 16 bits (e.g. `static unsigned long g = 0x40400000UL;`) as
   a single `var NAME = <value>` line — but dgasm's `var` directive only
   ever reserves and deposits *one* word, silently truncating the value
   at parse time. This one was unusually hard to pin down because
   `0x40400000 mod 2^16` happens to be exactly `0`, which made a
   from-scratch-corrupted global look, from the *load* side alone,
   exactly like a plausible arithmetic bug instead of "this value was
   never really 32 bits in memory to begin with." Fixed by emitting one
   `var` line per word, most-significant-first, matching the existing
   array-initializer path.

6. **`ISD::BRCOND` needed `Custom` lowering, not `Expand`.** Relying on
   generic legalization to always eliminate `BRCOND` before instruction
   selection held for code shaped by this backend's own lowering, but
   broke once soft-float's more elaborate C (nested `if`/`else if`/`else`
   over 32-bit comparisons) gave the DAG combiner enough shape to rebuild
   a "Select feeding a brcond" pattern *after* the main BRCOND-Expand
   legalization pass had already run. Fixed by Custom-lowering `BRCOND`
   into the existing, already-robust `BR_CC` path instead, which gets
   re-invoked for any brcond node regardless of when it's introduced.

## A hard-won lesson on frame size

Several soft-float functions (`sf_add`, `__mulsf3`, `__divsf3`) needed
restructuring to fit this backend's ±127-word signed frame-relative
addressing limit — with only two allocatable registers (AC0/AC1), a
function juggling several 32-bit values racks up spill-slot pressure
fast. Three approaches were tried, in order:

1. **Output-pointer parameters** (helper functions report multiple
   results via `u32 *out`): made frames *bigger*, not smaller — every
   pointer read/write into an already-oversized frame goes through this
   backend's indirect-addressing workaround, which is itself expensive
   in words.
2. **File-scope statics for cross-call state**: got frames down to a
   manageable size, and is the approach that stuck — but only after bug
   #4/#5 above were actually fixed (an earlier attempt at this hit those
   bugs and looked, at the time, like statics themselves were unsafe for
   32-bit values — they were not; the underlying bugs were).
3. **Pure value parameters and single-value returns** (no pointers, no
   globals): tried as an alternative to #2, but cost *more* frame than
   either — keeping several 32-bit values simultaneously live across
   many sequential function calls (each call clobbers both allocatable
   registers, forcing spills) outweighed the savings from fewer named
   locals per function.

The winning pattern: narrow file-scope statics (`int` 0/1 flags instead
of full 32-bit masks where possible, fields reused across pipeline
stages rather than duplicated), split into `_extract` / `_align` (or
equivalent middle step) / `_finish` functions. See `eclipse_rt.c`'s
soft-float section for the actual code and its own inline comments.
