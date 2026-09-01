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
