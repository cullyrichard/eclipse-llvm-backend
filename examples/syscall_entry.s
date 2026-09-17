// syscall_entry.s -- hand-written Eclipse S/140 assembly, NOT compiled
// from C (see syscall.h's header comment for why). This IS the real SYC
// (System Call) trap handler: the code that runs at memory location 2's
// target once a user (or supervisor) program executes SYC. Installed by
// examples/syscall.c's syscall_install() (ELEF-computes this file's own
// __syscall_handler label into AC0, STA's it to location 2).
//
// NOT assembled standalone -- this file's text is meant to be
// concatenated onto the end of an eclipse-cc-style compiled program's
// own generated assembly, BEFORE reorder_asm.py runs, the exact same
// mechanism eclipse-cc already uses for
// eclipse-toolchain/rt/eclipse_hwfloat.s (see that file's own header
// comment: "eclipse-cc concatenates this file's contents onto the end
// of llc's own output ... before reorder_asm.py runs"). See
// SYSCALL_NOTES.md for the exact build recipe used to do this for
// syscall.c/console.c/blockdev.c/dsk.c + this file together (eclipse-cc
// itself has no flag for mixing a hand-written .s source in -- every
// "source" it accepts goes through clang -cc1, which cannot compile
// this file's own POPB/DIA/DOA/SYC-return-block-aware code).
//
// ============================================================
// THE DISCIPLINE THIS HANDLER FOLLOWS, verified (not re-derived) by
// TRAP_NOTES.md's "SYC and MapIntMode" section (examples/
// syc_intmode_probe.s) and reused here unchanged:
// ============================================================
//   1. Capture AC0 (syscall reason) / AC1 (arg1) / AC2 (arg2)
//      IMMEDIATELY -- before anything else, DIA included, can disturb
//      them.
//   2. DIA 0,MAP -- the FIRST MAP-device access of any kind since SYC
//      fired, before any other MAP-device I/O, full stop. Bits 1-15 of
//      the result (A/B select, bit 13, included) are the pre-trap
//      MapStat bit-for-bit, because SYC's own dispatch clears only bit
//      0 (`MapStat &= ~1` -- TRAP_NOTES.md, confirmed against
//      eclipse_cpu.c). Bit 0 itself is NOT trustworthy from this read
//      (SYC never populates MapIntMode, unlike a generic device
//      interrupt) -- reconstructed explicitly in step 5 below instead.
//   3. Hand off to compiled C (`syscall_dispatch(reason)`) to do the
//      real work, via the ordinary SAVE/RTN calling convention's own
//      EJSR-based call/push/pop -- the exact shape
//      eclipse-toolchain/rt/eclipse_hwfloat.s's own EJSR calls into
//      ieee754_to_hexfloat32/hexfloat32_to_ieee754/__hwf_fits_pos16
//      already establish and verify (push argument(s) via
//      `ISZ 040,0` + `STA reg,@040`, `EJSR target,0`, pop via
//      `DSZ 040,0` per argument), reduced here to this ABI's own
//      1-argument case (syscall_dispatch takes exactly one int,
//      `reason` -- sysarg1/sysarg2 cross this same boundary through
//      plain globals instead, exactly the way eclipse-toolchain's own
//      hand-written-asm-calling-compiled-C precedent (examples/mmpu.c's
//      header comment) already established for this backend: route
//      values through named globals rather than trust operand-level
//      register placement for anything hardcoded-by-convention).
//   4. Patch the SYC-pushed stack's AC0 slot with syscall_dispatch's
//      return value -- trap_probe.s's own verified "POPB restores from
//      the STACK, not live registers" fixup (`STA 0,-4,2` off a freshly
//      reloaded AC2 = the post-push stack pointer). AC2 is reloaded
//      FRESH, AFTER the call returns, not reused from step 1's capture:
//      syscall_dispatch's own compiled prologue (`SAVE n` / `MOV 3,2`)
//      clobbers AC2 as ITS OWN frame pointer while it runs, exactly the
//      same "AC2 is the live frame pointer, and an ordinary call does
//      not preserve the caller's own value in it" hazard examples/
//      mmpu.c's header comment already found and saved/restored around
//      for LMP; here the call itself is the clobbering event, so the
//      fix is simply "reload AC2 = mem[040] again after the call", not
//      a save/restore (nothing before the call needs AC2's original
//      value again).
//   5. Reconstruct MapStat: bits 1-15 from the saved DIA readback (step
//      2), bit 0 forced ON explicitly -- never trusted from DIA. This
//      is safe because bit 0's true pre-trap value is not actually
//      unknown: SYC only reaches this handler with a real user-mode
//      trap (the case this ABI cares about) when MapStat bit 0 was
//      already 1 to get there (TRAP_NOTES.md). DOA that reconstructed
//      value back.
//   6. POPB -- pops AC0-AC3+PC(+carry), reactivates Usermap per the
//      DOA just issued (step 5), resumes at the instruction after SYC.
//
// ============================================================
// THE SYC-VS-SCHEDULER RACE FIX (SYSCALL_NOTES.md section 7): mask ALL
// device interrupts for this handler's ENTIRE duration, not just SYC's
// own automatic 2-instruction Inhibit window.
// ============================================================
// SYSCALL_NOTES.md's own verified-race section found that MapStat bit 0
// (User Enable) reads as 0 for a syscall's WHOLE duration (SYC clears
// it; nothing sets it again until step 5/6 above), so a PIT interrupt
// landing anywhere in this handler makes scheduler_probe.s's INTHANDLER
// misidentify -- and sometimes corrupt -- whichever process is
// resident. The fix here does not touch INTHANDLER or the TCB at all:
// it makes the race impossible to enter in the first place, using a
// real, documented, general-purpose interrupt mask distinct from SYC's
// own narrow Inhibit -- Data General 014-000642-02 Rev 02, p.5-77/5-78
// (PDF pages 83-84), "Interrupt Disable" (`INTDS` / `NIOC CPU` -- "Sets
// Interrupt On flag to 0") and "Interrupt Enable" (`INTEN` / `NIOS CPU`
// -- "Sets Interrupt On flag to 1[;] the CPU allows one more instruction
// to execute before the first I/O interrupt can occur"). `examples/
// scheduler_probe.s` already relies on this exact ION mechanism (its
// own `NIOS 077` as literally the last instruction before `EJMP
// @resume_target`, and PROCESS_NOTES.md section 2's own "int_req >
// INT_PENDING is algebraically false while ION is off" derivation) --
// this fix is the same real hardware primitive, reused, not a new one.
//
// Cross-checked directly against `eclipse_cpu.c`'s `DEV_CPU` (device
// 077) dispatch (~line 5143-5149), not assumed from the manual alone:
//   case iopS: int_req = (int_req | INT_ION) & ~INT_NO_ION_PENDING; break; // NIOS 077 / INTEN
//   case iopC: int_req = int_req & ~INT_ION; break;                        // NIOC 077 / INTDS
// `INTDS`/`NIOC 077` clears bit 19 (`INT_ION`) immediately, with no
// delay bit of its own (unlike enable, it never touches
// `INT_NO_ION_PENDING`) -- and the main dispatch loop's interrupt gate
// (`int_req > INT_PENDING && !Inhibit`, `INT_PENDING = INT_ION +
// INT_NO_ION_PENDING`) can never be satisfied with `INT_ION` clear, no
// matter what `INT_NO_ION_PENDING` or the device bits hold (their
// combined maximum, `INT_NO_ION_PENDING | INT_DEV | INT_STK` = 524287,
// is strictly less than `INT_PENDING` = 786432) -- so once `NIOC 077`
// runs, NOTHING can preempt this handler, for as long as it runs,
// until this handler's own `NIOS 077` reinstates `INT_ION`. `pit_svc`
// (~line 5862-5871) sets `dev_done`/`int_req`'s PIT bit completely
// independently of `INT_ION` -- a PIT wrap during the mask is not
// lost, only deferred: it becomes pending and is delivered at the
// first safe instruction boundary once ION is back on.
//
// Placement, verified against the same iteration-by-iteration `Inhibit`
// trace SYSCALL_NOTES.md's own race section already used:
//   - `NIOC 077` is this handler's OWN FIRST INSTRUCTION. There is no
//     gap between SYC's trap and this mask taking effect: SYC's own
//     dispatch sets `Inhibit=3` as its last act (blocking the interrupt
//     check for this handler's first instruction unconditionally,
//     regardless of ION), and `NIOC 077`'s own effect on `int_req` is
//     immediate (no enable-side delay), so by the time `Inhibit` lapses
//     (before this handler's 3rd instruction), ION is already off.
//   - `NIOS 077` is the LAST instruction before `POPB` -- the identical
//     "NIOS then exactly one more instruction" idiom `scheduler_probe.s`
//     already uses before `EJMP @resume_target`. Here the guaranteed
//     next instruction is `POPB` itself: a pending, deferred PIT
//     interrupt (if the timer wrapped while masked) can only be
//     delivered starting at the FIRST instruction after this syscall's
//     own caller resumes -- an ordinary, already-verified preemption
//     point (PROCESS_NOTES.md), never mid-handler.
//   - Neither `NIOC 077` nor `NIOS 077` touches `MapStat`/`MapIntMode`
//     at all (confirmed directly: `DEV_CPU`'s dispatch code never
//     references either) -- fully orthogonal to, and does not disturb,
//     the DIA-must-be-first / bit-0-reconstruction discipline above.
//
// What this fix costs, honestly: while ION is masked, a genuinely
// preemptable "other" process gets NO timeslice at all for this
// syscall's full duration, however long it runs (a real cost for a
// long polling-loop syscall like block I/O) -- a latency/fairness
// tradeoff, not a correctness one. See SYSCALL_NOTES.md for the
// alternative (per-process software state tracking) this was weighed
// against and why this was chosen instead.
//
// See SYSCALL_NOTES.md for the real, single-stepped SIMH trace evidence
// this handler was verified with, including the scheduler-race-specific
// verification this file exists to make possible (not just a rehash of
// syc_intmode_probe.s's own already-verified finding), and the fix's
// own re-verification against the exact race previously demonstrated.

	dev MAP = 03

__syscall_handler:
	NIOC 077			// INTDS -- mask ALL device interrupts
					// for this handler's entire duration;
					// see header section above. Covered by
					// SYC's own Inhibit=3 for this single
					// instruction, so there is no window
					// before the mask itself takes effect.
	ESTA 0, __sysh_reason
	ESTA 1, __sysh_arg1
	ESTA 2, __sysh_arg2

	DIA 0, MAP			// FIRST MAP-device access -- see header
	ESTA 0, __sysh_mapstat_raw

	// ---- hand off sysarg1/sysarg2 for syscall_dispatch() to read,
	// then make the actual call: syscall_dispatch(reason) ----
	ELDA 0, __sysh_arg1
	ESTA 0, sysarg1
	ELDA 0, __sysh_arg2
	ESTA 0, sysarg2

	ELDA 0, __sysh_reason
	ISZ 040, 0
	STA 0, @040
	EJSR syscall_dispatch, 0
	DSZ 040, 0

	// AC2 is now whatever syscall_dispatch's own frame pointer left
	// behind (see header point 4) -- reload the real, current
	// (post-SYC-push) stack pointer fresh before using AC2 again.
	LDA 2, 040
	STA 0, -4, 2			// patch the pushed AC0 slot with the
					// real return value (trap_probe.s's
					// own verified fixup)

	// ---- reconstruct MapStat (header point 5), resume ----
	ELDA 0, __sysh_mapstat_raw
	ELDA 1, __sysh_mask_no_bit0
	AND 1, 0			// AC0 := AC1 AND AC0 -- forces bit 0
					// to 0 first (safe against carry into
					// bit 1 from the ADI below)
	ADI 0, 1			// AC0 += 1 -- sets bit 0 (known-good
					// constant, not read from DIA)
	DOA 0, MAP
	NIOS 077			// INTEN -- unmask interrupts; see
					// header section above for why POPB
					// (the guaranteed next instruction) is
					// always safe to take a deferred
					// interrupt after, never before.
	POPB

	var __sysh_mask_no_bit0 = 0177776	// all bits except bit 0
	var __sysh_reason = 0
	var __sysh_arg1 = 0
	var __sysh_arg2 = 0
	var __sysh_mapstat_raw = 0
