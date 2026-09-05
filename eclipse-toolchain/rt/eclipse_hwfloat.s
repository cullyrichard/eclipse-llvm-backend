// eclipse_hwfloat.s -- hand-written Eclipse S/140 assembly, NOT compiled
// from C. Implements __addsf3_hw/__subsf3_hw: alternate ADD_F32/SUB_F32
// libcall implementations selected only when the backend's optional
// `+hwfloat` SubtargetFeature is enabled (see EclipseISelLowering.cpp's
// constructor and EclipseSubtarget::initLibcallLoweringInfo in the
// llvm-project checkout, and eclipse-cc's --hwfloat flag). The default,
// unconditional path for every other invocation of this toolchain is
// still the pure-software __addsf3/__subsf3 in eclipse_rt.c -- this file
// is only ever assembled into the final program when --hwfloat is passed.
//
// WHY THIS IS HAND-WRITTEN ASSEMBLY, NOT C: the LLVM backend has no
// SelectionDAG-level support for emitting FLDS/FAS/FSTS (no f32 register
// class, no Custom lowering for float ops -- see EclipseISelLowering.h's
// header comment) and Clang has no inline-asm support for this target.
// So these two functions are written directly against the real Eclipse
// S/140 FPU instruction set and dropped into the assembled program
// textually (eclipse-cc concatenates this file's contents onto the end
// of llc's own output, before reorder_asm.py runs -- see its own
// comment for why that's safe: reorder_asm.py is pure line-oriented
// text manipulation, agnostic to where a `var`/label/instruction line
// originally came from).
//
// PAGE-ZERO ADDRESSING GOTCHA (found the hard way -- see
// DEBUGGING_NOTES.md): reorder_asm.py's PZ_VAR_RE only hoists `var`
// lines matching the compiler's own pointer-slot/constant-pool naming
// conventions (`*_SLOT`/`*_PTR`/`*_offN`/`CPI<n>_<m>`) into page-zero --
// every other `var` (this file's own `__hwf_*` scratch included) is
// "bulk" data left wherever it lands, which on any nontrivial linked
// program is far past page-zero's 0-255 range (see entry #23 in
// DEBUGGING_NOTES.md: globals haven't lived in page-zero by default
// since the ELEF/ELDA/ESTA redesign). A plain `LDA`/`STA` can only
// reach page-zero -- confirmed empirically ("Address out of range. Got
// 5368, should be 0 - 255") the first time this file used them against
// its own scratch vars. Fixed by using `ELDA`/`ESTA` (extended/absolute
// addressing, real 2-word instructions reaching the full 16-bit space)
// for every access to `__hwf_*`, exactly like the compiler's own
// generated code already does for every other bulk global. FLDS/FSTS
// themselves are unaffected -- confirmed via the same probe that this
// gotcha wasn't about them: their `__hwf_*` operands (also far past
// page-zero) produced no assembler complaint at all, consistent with
// FLDS/FSTS being genuine 2-word extended-addressing instructions in
// their own right (matching their own "confirmed real hardware, not a
// page-zero-only instruction" evidence in DEBUGGING_NOTES.md).
//
// *** ARGUMENT-ORDER GOTCHAS (two separate, easy-to-conflate bugs found
// the hard way while bringing this file up -- see DEBUGGING_NOTES.md
// for the full empirical derivation of both): ***
//
// (1) WORD ORDER: a 32-bit value's LOW word is pushed/read first, HIGH
//     word second -- the opposite of the natural-looking assumption.
//     Confirmed with a dedicated, assumption-free probe (two trivial C
//     functions, get_hi16(x)/get_lo16(x), called with two
//     distinguishable pushed words). RETURN VALUES go the other way:
//     AC0 holds the HIGH word, AC1 the LOW word.
//
// (2) ARGUMENT ORDER: for a *multi-argument* call, arguments are pushed
//     right-to-left (SOFT_FLOAT_NOTES.md already documented this for
//     the software stack in general -- easy to read past as a detail
//     that wouldn't matter here, but it does): for a 2-arg function
//     f(a, b), the SECOND source argument (b) is pushed FIRST, landing
//     at FP-8/FP-7; the FIRST source argument (a) is pushed SECOND,
//     landing at FP-6/FP-5. Confirmed empirically by deliberately
//     reversing a hand-assembled test harness's push order for
//     __subsf3_hw and reproducing the exact same wrong-sign bug this
//     entry describes.
//
// Getting either backwards doesn't fail loudly. Word order (1): this
// file's first version silently fed ieee754_to_hexfloat32 the wrong
// 32-bit value (mantissa bits where sign+exponent were expected),
// which its own `exp8 == 0` subnormal-input guard happened to catch
// and flush to zero rather than crash -- so every hwfloat add/subtract
// silently produced 0 instead of an assembler/runtime error. Argument
// order (2): __addsf3_hw is commutative (a+b == b+a), so swapping which
// physical slot is "a" and which is "b" is invisible there -- it only
// surfaced as a bug in __subsf3_hw, which silently computed b-a instead
// of a-b for every call with two genuinely different operands (easy to
// miss with test values picked so a-b and b-a look superficially
// plausible; only caught because a wider verification sweep included
// operand pairs where the wrong sign was unambiguous).
//
// Also note: word order (1) is about how a *single* 32-bit value's own
// two words are ordered, which is unrelated to and must not be
// conflated with how *this file's own* hex-float bit pattern is laid
// out in memory (FLDS/FSTS's two-word operand is plain most-
// significant-word-first, verified separately in the format reverse-
// engineering, and completely unaffected by either calling-convention
// gotcha above).
//
// CALLING CONVENTION (mechanics, independent of the word-order
// question above): exactly this backend's ordinary one (see
// EclipseFrameLowering.cpp/EclipseISelLowering.cpp's LowerCall/
// LowerFormalArguments/LowerReturn, and DEBUGGING_NOTES.md entry #11) --
// these two functions are indistinguishable, from a caller's point of
// view, from an ordinary compiler-generated `float f(float,float)`
// function, called via the compiler's normal EJSR sequence -- and they
// use that same convention themselves to call the two IEEE<->hex-float
// conversion helpers below (ordinary C functions, compiled normally --
// see eclipse_rt.c). Verified directly (not just by reading the
// convention's own comments) with hand-assembled harnesses that push
// arguments through the real hardware stack (mem[040]) exactly the way
// compiler-generated code does, call via EJSR, and check the result --
// see DEBUGGING_NOTES.md for the full methodology:
//   - The 32-bit return value goes out through AC0/AC1 (word order per
//     the gotcha above), written to the fixed slots FP-4 (AC0) / FP-3
//     (AC1) immediately before RTN -- RTN itself (real hardware
//     SAVE/RTN, not a software stack) restores AC0/AC1 from those exact
//     slots as it pops the frame, handing the caller back its result in
//     AC0:AC1. This holds just as well for a *nested* call made from
//     inside one of these functions (calling the conversion helpers
//     below) as it does for the outermost call from compiler-generated
//     code -- ordinary nested-call behavior, ultimately no different
//     from any other function calling another function on this target.
//   - Arguments are pushed onto the real hardware stack (mem[040], the
//     same fixed location SAVE/RTN themselves use -- EclipseISelLowering
//     .cpp's HWStackAddr) via increment-then-store (`ISZ 040,0` /
//     `STA reg,@040`), and popped via plain decrement (`DSZ 040,0`) --
//     matching EclipseISelLowering.cpp's emitPushPop/emitAdjCallStack
//     exactly, including their upward-growth direction (opposite the
//     old, no-longer-used software-only `_SP` stack). `@040` is plain
//     page-zero indirection (address 040 itself, not the value stored
//     there, is always page-zero), so this part stays plain STA, not
//     ESTA.
//   - `SAVE 4` / `MOV 3,2` is the ordinary compiler-generated prologue
//     shape (SAVE establishes the new frame and leaves it in AC3; MOV
//     copies it into AC2, this backend's fixed frame-pointer register).
//     The `4` is arbitrary local-scratch headroom SAVE reserves --
//     unused here since both functions route all their own scratch
//     through the fixed page-zero cells below instead of frame-relative
//     locals, but a real SAVE argument is needed for RTN's own frame
//     teardown to behave like every other function's.
//
// HARDWARE FLOAT FORMAT (reverse-engineered, see DEBUGGING_NOTES.md for
// the full evidence): the Eclipse S/140 FPU's "short" (single-precision,
// 2-word/32-bit) format is IBM System/360-style hex-float -- 1 sign bit,
// a 7-bit exponent in excess-64 notation over powers of 16 (not 2), and
// a 24-bit mantissa normalized to a hex-digit (4-bit) boundary. This is
// a *different* representation from the IEEE-754 single-precision format
// this target's `float` uses everywhere else (its C ABI, printf/
// print_float, struct layout, the existing software __addsf3, ...) --
// deliberately NOT changed by this feature, so nothing outside these two
// functions' own bodies ever needs to know the hardware format exists.
// ieee754_to_hexfloat32/hexfloat32_to_ieee754 (eclipse_rt.c) bridge that
// gap; FAS/FSS themselves, this calling-convention wrapper shape, and
// the conversion pair are all separately verified on eclipseemu -- see
// DEBUGGING_NOTES.md for the full results.

__addsf3_hw:
	SAVE 4
	MOV 3,2
	LDA 0,-8,2
	ESTA 0,__hwf_b_lo
	LDA 0,-7,2
	ESTA 0,__hwf_b_hi
	LDA 0,-6,2
	ESTA 0,__hwf_a_lo
	LDA 0,-5,2
	ESTA 0,__hwf_a_hi
	ELDA 0,__hwf_a_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_a_hi
	ISZ 040,0
	STA 0,@040
	EJSR ieee754_to_hexfloat32,0
	DSZ 040,0
	DSZ 040,0
	ESTA 0,__hwf_a_hi
	ESTA 1,__hwf_a_lo
	ELDA 0,__hwf_b_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_b_hi
	ISZ 040,0
	STA 0,@040
	EJSR ieee754_to_hexfloat32,0
	DSZ 040,0
	DSZ 040,0
	ESTA 0,__hwf_b_hi
	ESTA 1,__hwf_b_lo
	FLDS 0,__hwf_a_hi
	FLDS 1,__hwf_b_hi
	FAS 0,1
	FSTS 1,__hwf_r_hi
	ELDA 0,__hwf_r_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_r_hi
	ISZ 040,0
	STA 0,@040
	EJSR hexfloat32_to_ieee754,0
	DSZ 040,0
	DSZ 040,0
	STA 0,-4,2
	STA 1,-3,2
	RTN

// __subsf3(a, b) must compute a - b, not b - a -- FSS's "dst -= src"
// convention (same accumulate-in-place shape as the integer SUBrr this
// mirrors) means the MINUEND has to be loaded into the destination
// accumulator (AC1) and the SUBTRAHEND into the source (AC0), i.e. the
// *opposite* load order from __addsf3_hw above (where operand order
// doesn't matter -- addition is commutative). Verified against both
// operand orderings on eclipseemu before picking this one -- see
// DEBUGGING_NOTES.md.
__subsf3_hw:
	SAVE 4
	MOV 3,2
	LDA 0,-8,2
	ESTA 0,__hwf_b_lo
	LDA 0,-7,2
	ESTA 0,__hwf_b_hi
	LDA 0,-6,2
	ESTA 0,__hwf_a_lo
	LDA 0,-5,2
	ESTA 0,__hwf_a_hi
	ELDA 0,__hwf_a_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_a_hi
	ISZ 040,0
	STA 0,@040
	EJSR ieee754_to_hexfloat32,0
	DSZ 040,0
	DSZ 040,0
	ESTA 0,__hwf_a_hi
	ESTA 1,__hwf_a_lo
	ELDA 0,__hwf_b_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_b_hi
	ISZ 040,0
	STA 0,@040
	EJSR ieee754_to_hexfloat32,0
	DSZ 040,0
	DSZ 040,0
	ESTA 0,__hwf_b_hi
	ESTA 1,__hwf_b_lo
	FLDS 0,__hwf_b_hi
	FLDS 1,__hwf_a_hi
	FSS 0,1
	FSTS 1,__hwf_r_hi
	ELDA 0,__hwf_r_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_r_hi
	ISZ 040,0
	STA 0,@040
	EJSR hexfloat32_to_ieee754,0
	DSZ 040,0
	DSZ 040,0
	STA 0,-4,2
	STA 1,-3,2
	RTN

// __eqsf2_hw/__nesf2_hw/__ltsf2_hw/__lesf2_hw/__gtsf2_hw/__gesf2_hw --
// hardware-backed alternates for the six three-way (-1/0/1) comparison
// libcalls (see eclipse_rt.c's sf_cmp comment for the convention: all
// six are thin aliases of ONE shared -1/0/1 result, tested afterward by
// the generic softenSetCCOperands with a per-predicate CondCode against
// 0 -- so all six genuinely need only one real body here too;
// __eqsf2_hw is the only one with real code, the other five are plain
// unconditional jumps into it, safe because the calling convention puts
// every argument in the exact same frame-relative slots no matter which
// symbol name the caller actually invoked).
//
// Design: compute diff = a - b using the SAME real hardware FSS
// subtraction __subsf3_hw above already uses (already verified
// correct), then use FCMP plus a skip-family instruction (FSEQ/FSGT) to
// read diff's sign/zero state. NOT a direct two-operand float compare
// -- FCMP was characterized here for the first time (DEBUGGING_NOTES.md
// entry #27/#30 only confirmed it "executes without error", never what
// it actually does), and it turns out to NOT genuinely compare its two
// operands against each other on eclipseemu at all: probing several
// independent pairs (2.0 vs 3.0, 3.0 vs 2.0, 2.0 vs 2.0, 0.0 vs 0.0,
// -2.0 vs 2.0, 5.0 vs 0.0, 2.0 vs -5.0, 0.0 vs 5.0) showed FCMP's
// condition flags depend ONLY on its SECOND operand's own sign/zero
// state -- the first operand is read but has zero effect on the
// result. (Confirmed directly and unambiguously: `FCMP 0,1` with
// AC0=2.0,AC1=0.0 and AC0=3.0,AC1=0.0 -- genuinely different first
// operands -- produced the identical "equal" result both times, purely
// because AC1==0.0 in both; `FCMP 1,0` with the operand *positions*
// swapped correctly switched to testing AC0 instead, confirming it's
// positional -- whichever operand is written second -- not tied to a
// specific register number.) That's still exactly what a real
// two-operand compare needs once the operands are first combined by a
// genuine subtraction -- which this design has to do anyway to get
// their sign/magnitude relationship, so it costs nothing extra over a
// direct (nonexistent) two-operand compare. The reverse
// hexfloat32_to_ieee754 conversion __addsf3_hw/__subsf3_hw need for
// their own result is NOT needed here -- only the SIGN of the diff
// matters, never its numeric value, so this is actually cheaper than
// add/subtract's own hardware path.
//
// Negative-zero caveat, checked and found not to apply here: FCMP/FSEQ
// does NOT treat the raw hex-float bit pattern for negative zero (sign
// bit set, all-else zero) as equal to positive zero when that pattern
// is loaded directly (confirmed with a dedicated probe). But this
// design never loads that pattern directly -- it only ever tests the
// output of a genuine FSS subtraction, which (confirmed directly: FSS
// 2.0-2.0 and FSS 0.0-0.0 both) always produces the canonical
// all-zero-bits result for a truly-zero difference, so the raw-
// negative-zero edge case never actually arises here. (One narrow
// residual gap, honestly noted: comparing two floats that are each
// already exactly +0.0/-0.0 *by construction* rather than by
// subtracting to zero is covered by this same argument -- FSS's own
// zero output is canonical regardless of its two input signs -- but
// wasn't separately reprobed with a *negative*-zero operand feeding the
// subtraction; not expected to differ, since FSS's zero-producing path
// doesn't depend on which operand signs produced the zero, only that
// the true mathematical difference is zero.)
__eqsf2_hw:
	SAVE 4
	MOV 3,2
	LDA 0,-8,2
	ESTA 0,__hwf_cmp_b_lo
	LDA 0,-7,2
	ESTA 0,__hwf_cmp_b_hi
	LDA 0,-6,2
	ESTA 0,__hwf_cmp_a_lo
	LDA 0,-5,2
	ESTA 0,__hwf_cmp_a_hi
	ELDA 0,__hwf_cmp_a_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cmp_a_hi
	ISZ 040,0
	STA 0,@040
	EJSR ieee754_to_hexfloat32,0
	DSZ 040,0
	DSZ 040,0
	ESTA 0,__hwf_cmp_a_hi
	ESTA 1,__hwf_cmp_a_lo
	ELDA 0,__hwf_cmp_b_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cmp_b_hi
	ISZ 040,0
	STA 0,@040
	EJSR ieee754_to_hexfloat32,0
	DSZ 040,0
	DSZ 040,0
	ESTA 0,__hwf_cmp_b_hi
	ESTA 1,__hwf_cmp_b_lo
	FLDS 0,__hwf_cmp_b_hi
	FLDS 1,__hwf_cmp_a_hi
	FSS 0,1
	FCMP 0,1
	FSEQ
	JMP __hwf_cmp_ne
	ELDA 0,__hwf_cmp_zero
	JMP __hwf_cmp_done
__hwf_cmp_ne:
	FCMP 0,1
	FSGT
	JMP __hwf_cmp_notgt
	ELDA 0,__hwf_cmp_plus1
	JMP __hwf_cmp_done
__hwf_cmp_notgt:
	ELDA 0,__hwf_cmp_minus1
__hwf_cmp_done:
	STA 0,-4,2
	STA 0,-3,2
	RTN

__nesf2_hw:
	JMP __eqsf2_hw
__ltsf2_hw:
	JMP __eqsf2_hw
__lesf2_hw:
	JMP __eqsf2_hw
__gtsf2_hw:
	JMP __eqsf2_hw
__gesf2_hw:
	JMP __eqsf2_hw

// __floatsisf_hw(i)/__floatunsisf_hw(mag) -- hardware-backed alternates
// for int->float conversion, via FLAS. Guarded by __hwf_fits_pos16
// (eclipse_rt.c -- see its own comment for the full empirical story:
// FLAS turned out reliable ONLY for a value in [0,32767], including for
// the *signed* direction -- every negative value tried came back
// wildly wrong, not just the ones outside 16-bit range) and falling
// back to the exact original software implementation (__floatsisf/
// __floatunsisf, both completely unmodified) whenever the guard fails,
// so these are correct for every input, faster only for the common
// (small nonnegative) case. Both functions are otherwise identical
// except which software fallback they call -- not merged into one
// shared body the way the six comparisons above are, since unlike
// those, __floatsisf_hw/__floatunsisf_hw are NOT interchangeable (their
// fallback path genuinely differs).
__floatsisf_hw:
	SAVE 4
	MOV 3,2
	LDA 0,-6,2
	ESTA 0,__hwf_cvi_lo
	LDA 0,-5,2
	ESTA 0,__hwf_cvi_hi
	ELDA 0,__hwf_cvi_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cvi_hi
	ISZ 040,0
	STA 0,@040
	EJSR __hwf_fits_pos16,0
	DSZ 040,0
	DSZ 040,0
	MOV 0,0,SZR
	JMP __hwf_fsi_fast
	JMP __hwf_fsi_soft
__hwf_fsi_fast:
	ELDA 0,__hwf_cvi_lo
	FLAS 0,0
	FSTS 0,__hwf_cvi_rhi
	ELDA 0,__hwf_cvi_rlo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cvi_rhi
	ISZ 040,0
	STA 0,@040
	EJSR hexfloat32_to_ieee754,0
	DSZ 040,0
	DSZ 040,0
	STA 0,-4,2
	STA 1,-3,2
	RTN
__hwf_fsi_soft:
	ELDA 0,__hwf_cvi_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cvi_hi
	ISZ 040,0
	STA 0,@040
	EJSR __floatsisf,0
	DSZ 040,0
	DSZ 040,0
	STA 0,-4,2
	STA 1,-3,2
	RTN

__floatunsisf_hw:
	SAVE 4
	MOV 3,2
	LDA 0,-6,2
	ESTA 0,__hwf_cvu_lo
	LDA 0,-5,2
	ESTA 0,__hwf_cvu_hi
	ELDA 0,__hwf_cvu_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cvu_hi
	ISZ 040,0
	STA 0,@040
	EJSR __hwf_fits_pos16,0
	DSZ 040,0
	DSZ 040,0
	MOV 0,0,SZR
	JMP __hwf_fui_fast
	JMP __hwf_fui_soft
__hwf_fui_fast:
	ELDA 0,__hwf_cvu_lo
	FLAS 0,0
	FSTS 0,__hwf_cvu_rhi
	ELDA 0,__hwf_cvu_rlo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cvu_rhi
	ISZ 040,0
	STA 0,@040
	EJSR hexfloat32_to_ieee754,0
	DSZ 040,0
	DSZ 040,0
	STA 0,-4,2
	STA 1,-3,2
	RTN
__hwf_fui_soft:
	ELDA 0,__hwf_cvu_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cvu_hi
	ISZ 040,0
	STA 0,@040
	EJSR __floatunsisf,0
	DSZ 040,0
	DSZ 040,0
	STA 0,-4,2
	STA 1,-3,2
	RTN

// __fixsfsi_hw(f)/__fixunssfsi_hw(f) -- hardware-backed alternates for
// float->int conversion, via FFAS. Guarded by __hwf_fits_i16f/
// __hwf_fits_u16f (eclipse_rt.c -- see its own comment: FFAS turned out
// reliable across the FULL signed 16-bit range, including negative
// values and the -32768 boundary -- unlike FLAS's asymmetric failure
// above -- but unreliable once the true magnitude reaches 32768 or
// beyond), falling back to the exact original software implementation
// (__fixsfsi/__fixunssfsi, both completely unmodified) whenever the
// guard fails.
//
// __fixsfsi_hw needs one extra step __fixunssfsi_hw doesn't: FFAS's
// 16-bit signed AC result has to be sign-extended into this function's
// real 32-bit `long` return. AC3 is used as disposable scratch for the
// sign test (`AND acS,acD,SZR` overwrites acD, so the sign test can't
// run directly on AC1 without destroying the very result it's testing)
// -- AC1 itself is never written again after FFAS, so it still holds
// the correct low word throughout. __fixunssfsi_hw needs no such step:
// its guard already guarantees a nonnegative result under 32768, so the
// high word is unconditionally 0 (plain zero-extension).
__fixsfsi_hw:
	SAVE 4
	MOV 3,2
	LDA 0,-6,2
	ESTA 0,__hwf_cvf_lo
	LDA 0,-5,2
	ESTA 0,__hwf_cvf_hi
	ELDA 0,__hwf_cvf_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cvf_hi
	ISZ 040,0
	STA 0,@040
	EJSR __hwf_fits_i16f,0
	DSZ 040,0
	DSZ 040,0
	MOV 0,0,SZR
	JMP __hwf_fsf_fast
	JMP __hwf_fsf_soft
__hwf_fsf_fast:
	ELDA 0,__hwf_cvf_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cvf_hi
	ISZ 040,0
	STA 0,@040
	EJSR ieee754_to_hexfloat32,0
	DSZ 040,0
	DSZ 040,0
	ESTA 0,__hwf_cvf_hi
	ESTA 1,__hwf_cvf_lo
	FLDS 0,__hwf_cvf_hi
	FFAS 1,0
	MOV 1,3
	ELDA 0,__hwf_mask8000
	AND 0,3,SZR
	JMP __hwf_fsf_neg
	ELDA 0,__hwf_zero
	JMP __hwf_fsf_setlo
__hwf_fsf_neg:
	ELDA 0,__hwf_allones
__hwf_fsf_setlo:
	STA 0,-4,2
	STA 1,-3,2
	RTN
__hwf_fsf_soft:
	ELDA 0,__hwf_cvf_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cvf_hi
	ISZ 040,0
	STA 0,@040
	EJSR __fixsfsi,0
	DSZ 040,0
	DSZ 040,0
	STA 0,-4,2
	STA 1,-3,2
	RTN

__fixunssfsi_hw:
	SAVE 4
	MOV 3,2
	LDA 0,-6,2
	ESTA 0,__hwf_cvfu_lo
	LDA 0,-5,2
	ESTA 0,__hwf_cvfu_hi
	ELDA 0,__hwf_cvfu_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cvfu_hi
	ISZ 040,0
	STA 0,@040
	EJSR __hwf_fits_u16f,0
	DSZ 040,0
	DSZ 040,0
	MOV 0,0,SZR
	JMP __hwf_fuf_fast
	JMP __hwf_fuf_soft
__hwf_fuf_fast:
	ELDA 0,__hwf_cvfu_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cvfu_hi
	ISZ 040,0
	STA 0,@040
	EJSR ieee754_to_hexfloat32,0
	DSZ 040,0
	DSZ 040,0
	ESTA 0,__hwf_cvfu_hi
	ESTA 1,__hwf_cvfu_lo
	FLDS 0,__hwf_cvfu_hi
	FFAS 1,0
	ELDA 0,__hwf_zerou
	STA 0,-4,2
	STA 1,-3,2
	RTN
__hwf_fuf_soft:
	ELDA 0,__hwf_cvfu_lo
	ISZ 040,0
	STA 0,@040
	ELDA 0,__hwf_cvfu_hi
	ISZ 040,0
	STA 0,@040
	EJSR __fixunssfsi,0
	DSZ 040,0
	DSZ 040,0
	STA 0,-4,2
	STA 1,-3,2
	RTN

// Shared scratch. Deliberately "bulk" (not page-zero -- see this file's
// header comment above), unlike most of this backend's own compiler-
// emitted scratch cells: nothing here needs the 8-bit-displacement
// reach a plain LDA/STA gets from page-zero, since every access already
// goes through ELDA/ESTA (or FLDS/FSTS, which reach the full address
// space natively) -- so there's no reason to spend this feature's own
// slice of the shared 256-word page-zero budget on words that don't
// need to be there. Safe to share between the two functions above:
// neither is reentrant/recursive and they're never executing
// concurrently (no interrupts enabled on this target during ordinary
// float arithmetic). Also safe across the nested calls to
// ieee754_to_hexfloat32/hexfloat32_to_ieee754 in between: those calls
// only ever clobber AC0-AC3 (this target's calling convention, same as
// any other call), never memory -- __hwf_a_hi etc. survive unchanged.
var __hwf_a_hi = 0
var __hwf_a_lo = 0
var __hwf_b_hi = 0
var __hwf_b_lo = 0
var __hwf_r_hi = 0
var __hwf_r_lo = 0

// Scratch for __eqsf2_hw (and the five plain-jump aliases into it) and
// the four int<->float conversion functions above -- same "bulk, safe
// to share, never reentrant/concurrent" reasoning as __hwf_a_hi etc.
// above, just split into separate names per function group rather than
// reused, purely so each new function's own code stays self-contained
// and easy to review (there is no page-zero budget cost either way --
// see this file's header comment on why "bulk" placement is free here).
var __hwf_cmp_a_hi = 0
var __hwf_cmp_a_lo = 0
var __hwf_cmp_b_hi = 0
var __hwf_cmp_b_lo = 0
var __hwf_cmp_zero = 0
var __hwf_cmp_plus1 = 1
var __hwf_cmp_minus1 = -1

var __hwf_cvi_hi = 0
var __hwf_cvi_lo = 0
var __hwf_cvi_rhi = 0
var __hwf_cvi_rlo = 0

var __hwf_cvu_hi = 0
var __hwf_cvu_lo = 0
var __hwf_cvu_rhi = 0
var __hwf_cvu_rlo = 0

var __hwf_cvf_hi = 0
var __hwf_cvf_lo = 0
var __hwf_mask8000 = 32768
var __hwf_zero = 0
var __hwf_allones = -1

var __hwf_cvfu_hi = 0
var __hwf_cvfu_lo = 0
var __hwf_zerou = 0
