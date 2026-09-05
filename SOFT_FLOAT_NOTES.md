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

`double` (f64) is **not** implemented as a genuine 64-bit type — instead,
`double` is a 32-bit alias for `float` on this target (`DoubleFormat =
IEEEsingle` in the Clang `TargetInfo`, the same choice AVR-GCC/Clang make
by default), since real 64-bit `double` would need genuine 64-bit integer
support this backend doesn't have (see the main backend `README.md`'s
"Known limitations"). This is what makes `float` arguments passed
through `printf(...)`'s varargs (which C always promotes to `double`)
work at all: `va_arg(ap, double)` reads the same 32-bit bit pattern
`float` already uses, no separate 64-bit path involved.

Printing a `float` is `print_float(f)` (declared in `stdio.h`), **not**
`printf("%f", f)` — see "Why print_float isn't wired into printf" below
for why that's a deliberate, budget-driven design choice, not a missing
feature.

## Hardware-accelerated path (default; `--ieee` opts out)

This target's Eclipse S/140 CPU turns out to have a genuine hardware
FPU (distinct from, and much more usable than, the external FPS100
I/O-device coprocessor documented in `DEBUGGING_NOTES.md`'s Background
section — `eclipseemu` cannot simulate the FPS100 at all, but does
simulate the CPU-native FPU). As of `DEBUGGING_NOTES.md` entry #30, real
hardware FPU instructions for `float` add/subtract/comparison and
int↔float conversion are the **default** — every existing invocation of
this toolchain that doesn't pass `--ieee` gets them, not just ones that
opt in. `--ieee` restores this target's *original*, fully-software
behavior byte-for-byte (the safety-net fallback everything above
describes); `--hwfloat` is an older, narrower flag kept for its own
meaning (see `eclipse-cc`'s own header comment) that now implies
`--ieee` too, so it reproduces its own pre-entry-#30 behavior exactly.

Covered by the hardware path (default, or `--hwfloat`):
- `+`/`-` (`FAS`/`FSS`, entry #27) — real hardware add/subtract.
- `==`/`!=`/`<`/`<=`/`>`/`>=` (entry #31) — real hardware compare, via a
  genuine `FSS` subtraction (`a - b`) whose sign is then read with
  `FCMP`+a skip instruction (`FSEQ`/`FSGT`/etc.) — not a direct
  two-operand `FCMP`, which turned out (entry #31) to only test its
  *second* operand's own sign against zero on `eclipseemu`, ignoring the
  first entirely.
- `(float)int`/`(float)unsigned`/`(int)float`/`(unsigned)float`
  (entry #31) — real hardware int↔float conversion (`FLAS`/`FFAS`), but
  only for a value the hardware is actually reliable for: `FLAS`
  (int→float) turned out reliable only for `[0,32767]` (every negative
  value tried came back wildly wrong — an asymmetry entry #30's own,
  positive-integers-only re-probe didn't catch), while `FFAS`
  (float→int) is reliable across the full signed 16-bit range including
  negative values and the `-32768` boundary, just not once the true
  magnitude reaches 32768. Every conversion outside its own safe range
  falls back to calling the exact original software implementation, so
  every input is still correct — just not always hardware-accelerated.

**Not** covered, in any mode: `*`/`/`. This target's real hardware
multiply/divide instructions (`FMS`/`FDS`, and the FPAC-by-memory
`FMMS`/`FDMS` forms) were probed across three independent sessions now
(entries #27, #30, #31 — the last specifically trying non-adjacent
register pairs, self-operand multiply/divide, and the memory-by-FPAC
forms, not just repeating the first two attempts) and found not
correctly simulated by `eclipseemu`: they execute without error but
always leave the destination `FPAC` at exactly zero. `*`/`/` keep using
the software implementation above unconditionally in every mode — a
documented, deliberate scope limit, not an oversight. See
`rt/eclipse_hwfloat.s` and `DEBUGGING_NOTES.md` entries #27/#30/#31 for
the full design, the reverse-engineered hardware number format (IBM
System/360-style hex float, *not* IEEE — bridged transparently at each
`_hw` function's own boundary, so `float`'s storage format/ABI stays
IEEE-754 everywhere else regardless of this path), and the calling-
convention gotchas found getting it right.

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

**`reorder_asm.py` now deduplicates constant-pool entries** (its
`dedup_constants` pass): LLVM's `MachineConstantPool` is *per function*,
so the same literal value used in several different functions — very
common across this file's `sf_add`/`sf_mul`/`sf_div`/`__fixsfsi`/
`print_float`/... helpers, which all reuse a small set of constants
(exponent bias, mask halves, loop bounds) — got a separate, independently
named page-zero word in each one, even though the value was identical.
Confirmed to reclaim 100+ duplicate words in a real program combining
several soft-float functions, and on its own resolved a real page-zero
overflow in a program combining device I/O, several `printf` calls, and
float conversion — see `dedup_constants`'s own comment in
`reorder_asm.py` for the full reasoning (why it's safe: every constant-
pool entry is exactly one 16-bit word with no multi-word grouping to
preserve, and every reference is a bare symbol operand with no offset
arithmetic against a neighbor). This is a strict improvement — it
reclaims genuine waste, not the floor cost of a program's actually-
unique data — so combining *many* distinct float capabilities in one
program can still exceed the budget; it just takes more to get there
now.

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

7. **Storing a 32-bit value through a pointer *parameter* silently
   discards it.** Found while implementing `print_float`'s decimal-digit
   helper (`u32_div10`, originally `u32 u32_div10(u32 val, u32
   *rem_out)`): the quotient (an ordinary 2-word return) always came back
   correct, but `*rem_out = rem` never actually reached the caller's
   variable — read back as whatever was already there (0, in every test).
   Confirmed via an isolated repro (a single non-recursive call, no
   aliasing, no globals) that this is a genuine, previously-unexercised
   backend defect, not a logic bug in the caller. Nothing else in this
   file writes a wide value through a pointer *parameter* — the existing
   statics (`sf_add_aM` etc.) were adopted for frame-size reasons (see
   below), which happened to sidestep this too, so it went undetected
   until `print_float` specifically needed a "return two values" shape
   with a small enough footprint that pure statics-communication was the
   more natural choice. Worked around the same way: `u32_div10` now
   returns the quotient only and communicates the remainder through a
   static (`u32_div10_rem`) instead of a pointer. The actual root cause
   in SelectionDAG lowering was not isolated — flagged as a follow-up
   task, since it could affect other, not-yet-written code that
   legitimately wants an output-pointer parameter.

8. **A load-narrowing DAG combine silently misread wide-value bit
   tests.** `TargetLowering::SimplifySetCC` has a generic combine that
   rewrites `(wide_value & constant) == 0`-shaped comparisons into a
   narrower, byte-sized load at a shifted offset — a sound optimization
   on byte-addressable hardware, since only one byte of the wide value
   is ever tested. This backend has no byte-addressing instructions at
   all, and its declaration that an i8 `EXTLOAD`-from-i16 is `Legal` was
   only ever intended for genuine `char` values (which occupy a whole
   16-bit word here, top byte always zero) — not for the DAG combiner to
   reuse when narrowing into the *middle* of a wider (32-bit) multi-word
   value. Left unguarded, the synthesized narrow load returned the
   whole, unshifted word instead of the intended byte, while the
   combine's precomputed mask still targeted the low bits it *assumed*
   the load would land in — so `if (wide_var & constant)` silently
   evaluated the wrong way whenever the constant's set bit wasn't in the
   low byte (e.g. `if (mant_int & (1L << 27)) mant_int -= (1L << 28);`
   never took the branch, for any value of `mant_int`). Found while
   writing a standalone routine to decode a hardware FPU's raw
   fixed-point output back into a `float` — not part of the shipped
   soft-float runtime itself, but exactly the kind of 32-bit
   bit-manipulation code that exercises this class of bug the same way
   soft-float's own arithmetic does. Confirmed directly via `llc
   -debug-only=isel`: the combine rewrote `and i32 %load, 134217728`
   into `and i8 %narrowload, 8`, and the emitted `LDFI` loaded the
   unshifted high word regardless of the true value being tested. Fixed
   by overriding `TargetLowering::shouldReduceLoadWidth`
   (`EclipseISelLowering.h`) to veto the narrowing specifically when
   `NewVT == MVT::i8` — see that override's own comment for the full
   reasoning, including why every other width Eclipse actually uses
   (i16 reductions, and reducing a >16-bit load down to one of its
   already word-aligned i16 halves) is left untouched. Verified against
   a minimal repro, against the FPU-decode routine that surfaced it, and
   against a regression sweep of several existing example programs, all
   passing.

9. **The software stack's fixed origin had no margin against growing
   static data, so it could silently collide with it.** With only two
   allocatable registers (AC0/AC1), almost everything on this target
   spills to a software stack — a page-zero cell (`_SP`) holding the
   current top-of-stack address, pushed/popped via `STA n,@_SP` / `DSZ
   _SP,0`. `EclipseAsmPrinter::emitStartOfAsmFile` always hardcoded its
   *initial* value to `020000` (8192 decimal), chosen once with no
   relationship to how much data the final linked program would actually
   contain. Meanwhile `reorder_asm.py`'s `reorder()` deliberately moves
   every *bulk* (non-page-zero) global — every string literal and
   file-scope static — to the very end of the file, so dgasm deposits it
   at the *highest* addresses the program uses, an address that grows
   with the linked program's size (more functions pulled in, even ones
   genuinely dead at runtime — see `eclipse-cc`'s symbol-protection
   comment). Nothing ever compared that growing address against the fixed
   8192 stack origin sitting right above it: a small program leaves a
   large gap, but a large enough one shrinks that gap until ordinary
   call/recursion depth (e.g. `print_uint32`'s one-recursive-call-per-
   decimal-digit) drives the stack pointer down *into* that data —
   silently corrupting live globals and strings, and, once deep enough, a
   saved return address, causing execution to eventually run away to the
   reset vector. Found the same way as bug #8: an isolated repro
   (`calculate_value`/`scale_pow2`, this entry's own precursor) printed
   its first value correctly on its own, but broke — the very same
   `print_float` call, on the very same value — the instant a `<`
   comparison against it was *also* present later in `main`, even though
   the comparison's libcall (`__ltsf2`) hadn't been reached yet. That
   pointed away from `print_float`/`calculate_value`'s own logic (neither
   changes between the two programs) and toward something layout-
   dependent. Confirmed directly: examining word 0102 (the cell holding
   the live `_SP` value) mid-run on the failing program showed it
   reaching decimal 7605 while that program's own data extended to 7755 —
   the stack had already descended into live data before the eventual
   crash. dgasm itself never catches this: unlike the hard page-zero
   (0-255) ceiling it enforces for every `LDA`/`STA` displacement, it has
   no equivalent check for "does the stack collide with static data" — to
   dgasm, both are just plain memory words. Fixed in `reorder_asm.py`, not
   the backend: `EclipseAsmPrinter` emits `_SP`'s value long before the
   final program layout exists (before `dedup_constants`/
   `relax_long_jumps` run, and with no way to know what else will or
   won't end up linked in), so a new final pass, `fix_stack_pointer`,
   runs last of all — once the true end-of-program address is known — and
   repoints `_SP` comfortably (`STACK_MARGIN` = 4096 words) above it,
   instead of trusting a fixed guess; it fails loudly, instead of
   silently emitting an invalid address, if a program's own data ever
   grew large enough to leave no safe room (dgasm's own 65536-word address
   space ceiling, confirmed via `assembler.c`'s `MAX_MEMORY_WORDS`).
   Verified against both branches of the failing repro (the negative and
   positive `calculate_value`/`print_float`/`<`-compare paths) and a
   regression sweep of the existing example programs, all passing;
   `test_float.c`'s own documented page-zero-overflow failure (see "Known
   limit" above) is unaffected, since that's a separate, still-enforced
   budget this fix doesn't touch.

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
`print_float` needed the exact same treatment later (split into
`print_float_extract`/`print_float_frac`, communicating through
`pf_frac_bits`/`pf_fbits_n`) — confirming this isn't a one-off pattern
specific to the original three functions, but the standard shape any
sufficiently involved 32-bit-juggling function on this target needs.

## Why `print_float` isn't wired into `printf`'s `%f`

The first implementation put `%f` directly in `printf()`'s format-string
switch, calling `print_float` from there. This broke **every** program
that calls `printf()` at all, float or not: `printf()` is called by
nearly every program, and `internalize`/`globaldce` (see `eclipse-cc`'s
`build_and_assemble`) decides what's reachable from a plain, static IR
call graph — it can't see that a given call site's format string never
contains `"%f"`. A `printf` that calls `print_float` unconditionally
makes `print_float`, and everything it transitively calls, permanently
reachable from *any* program that calls `printf`. Confirmed empirically:
a bare `printf("%d\n", 42)` failed to assemble ("Address out of range"
on dozens of soft-float symbols) once `%f` lived inside `printf`'s
switch — the entire shared 256-word page-zero budget was gone before
`main` did anything.

This is fundamentally different from how the float-arithmetic RTLIB
calls (`__addsf3` etc.) stay invisible to non-float programs: `llc`
inserts *those* during instruction selection, after
internalize/globaldce has already run, so they're invisible to it either
way — exactly why `eclipse-cc`'s iterative "Undefined symbol" retry loop
exists (see its own comment). `print_float` is an ordinary, explicit C
call with no such trick available, so it has to stay opt-in: programs
call `print_float(f)` directly instead of `printf("%f", f)`. Non-float
programs (still the common case) pay nothing for its existence.

Even as a standalone function, `print_float` isn't free: it inherently
needs `__fixsfsi` (for the integer part) and, to avoid an even larger
cost, deliberately does *not* need `__subsf3`/`__mulsf3` (see its own
comment in `eclipse_rt.c` — the fraction is extracted directly from the
mantissa bits instead of via `(f - (float)(long)f) * 1000000.0f`, which
would have pulled in all of `sf_add` and `sf_mul` just to print a
number). Confirmed empirically that even `print_float` *alone*, with the
naive subtract-and-multiply approach, blew the page-zero budget on its
own — the mantissa-bit-extraction rewrite was necessary, not just an
optimization.
