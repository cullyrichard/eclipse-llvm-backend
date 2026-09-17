// syscall_sched_race_probe.s -- THE integration test this whole phase
// exists for: does a real SYC (syscall trap) survive a real PIT timer
// interrupt landing DURING its own execution, under
// examples/scheduler_probe.s's real preemptive 2-process scheduler?
// TRAP_NOTES.md's "SYC and MapIntMode" section explicitly left this
// open ("SYC racing a live, armed scheduler timer interrupt" was never
// tested) -- this file is that test.
//
// UPDATE (SYSCALL_NOTES.md section 7): the first version of this file
// found and demonstrated a real, corrupting race (Experiments 1/2 in
// SYSCALL_NOTES.md section 3, reproduced verbatim below in this file's
// own comments for the historical record). This file's own
// `__syscall_handler` copy has SINCE been updated in place with the
// real fix (`examples/syscall_entry.s`'s `NIOC 077`/`NIOS 077`
// ION-masking, copied verbatim -- see that file's header) -- this is
// the SAME probe, re-run at the SAME deterministic injection points,
// now demonstrating the race is closed rather than a new/different
// probe. See SYSCALL_NOTES.md section 7 for the fixed re-run
// transcripts and verdict.
//
// Built by combining, UNMODIFIED in mechanism, three already-verified
// pieces:
//   - examples/scheduler_probe.s's exact TCB layout (examples/proc.h),
//     2-process/map-A-map-B setup, and INTHANDLER (copied verbatim --
//     see INTHANDLER below -- this probe does NOT change the
//     scheduler's own logic in any way; the whole point is to test the
//     EXISTING, already-verified scheduler against a NEW situation it
//     was never exercised against).
//   - examples/syscall_entry.s's exact __syscall_handler body (copied
//     verbatim, same discipline: capture ACs, DIA-first, call dispatch,
//     reconstruct MapStat, POPB).
//   - A hand-written `syscall_dispatch` STAND-IN matching the real
//     eclipse-cc SAVE/RTN calling convention exactly (DEBUGGING_NOTES.md
//     entry #11) -- reads its one pushed argument at FP-5, records it,
//     returns a known counter value -- so __syscall_handler's real
//     EJSR/push/pop mechanics are exercised identically to how they
//     drive the REAL, compiled syscall_dispatch (examples/syscall.c),
//     without needing to pull the whole compiled-C pipeline into a
//     hand-assembled probe file just to test THIS mechanism.
//
// ============================================================
// THE SPECIFIC RISK UNDER TEST, worked out by reading eclipse_cpu.c's
// Inhibit mechanics directly (not assumed from TRAP_NOTES.md's own
// looser "more than one instruction" phrasing -- re-derived here since
// getting this exactly right is the whole point):
// ============================================================
// SYC sets Inhibit=3 as part of its own dispatch (eclipse_cpu.c
// ~line 1822). The main loop's Inhibit state machine
// (~line 835-839):
//     if (Inhibit == 3) Inhibit = 4;
//     if (Inhibit == 4) Inhibit = 0;
// runs ONCE PER INSTRUCTION, and the interrupt-pending check
// (~line 783, `if (int_req > INT_PENDING && !Inhibit)`) runs BEFORE
// that step, at the TOP of the SAME per-instruction loop iteration --
// so tracing the actual iteration-by-iteration sequence:
//   iteration K   (SYC itself):        Inhibit 0->0 (unchanged) trap dispatches, sets Inhibit=3 at its own end
//   iteration K+1 (handler instr #1):  interrupt check sees Inhibit=3 (blocked); Inhibit steps 3->4
//   iteration K+2 (handler instr #2):  interrupt check sees Inhibit=4 (blocked); Inhibit steps 4->0
//   iteration K+3 (handler instr #3):  interrupt check sees Inhibit=0 -- NOT BLOCKED
// So exactly the handler's first TWO instructions are protected --
// not "the handler's first instruction" as TRAP_NOTES.md's own closing
// section speculated, and not the whole handler. In
// examples/syscall_entry.s's __syscall_handler, that's:
//     ESTA 0, __sysh_reason     <- protected (instr #1)
//     ESTA 1, __sysh_arg1       <- protected (instr #2)
//     ESTA 2, __sysh_arg2       <- EXPOSED (instr #3)
//     DIA 0, MAP                <- EXPOSED (instr #4) -- THE
//                                   MapIntMode-contract-critical read
// So a PIT interrupt CAN land after arg1 is captured but before the
// handler's own DIA -- and, more broadly, MapStat's bit 0 (User
// Enable) stays 0 for the entire syscall (SYC clears it; nothing sets
// it again until the handler's own final DOA, just before POPB) -- so
// EVERY instruction of a syscall's execution, not just this narrow
// pre-DIA window, runs with bit 0 off. See "What this probe found"
// at the end of this file for why that matters more than the narrow
// window alone would.
//
// TOOLING: real, deterministic SCP-console PIT interrupt injection
// (`dep PIT DONE 1` / `dep PIT INT 1`), exactly MMPU_NOTES.md's
// "SCP register deposit as a deterministic interrupt trigger" section
// establishes and mmpu_intmode_resume_probe.s already uses -- lands an
// interrupt at an EXACT, chosen instruction boundary, repeatably, with
// no wall-clock dependency. This is the only way to reliably hit a
// 1-2 instruction window inside a syscall handler on demand; the real
// self-arming PIT (examples/pit_timer_probe.s, used unmodified by
// examples/scheduler_probe.s's own arm sequence below) is also armed
// here for realism (this probe is a real preemptive-scheduler program,
// not just a deposit-injection harness), but is NOT relied on for the
// actual race verification -- see SYSCALL_NOTES.md for the honest
// accounting of what real-timer (non-deterministic) coverage was and
// wasn't additionally run.
//
// See SYSCALL_NOTES.md for the real transcripts and the verdict.

	org 1
	var VECTOR = INTHANDLER

	org 050
_start:
	// ---- Hardware SAVE/RTN/POPB stack (DEBUGGING_NOTES.md entry #11,
	// TRAP_NOTES.md's own underflow-protection finding): this probe,
	// unlike scheduler_probe.s, uses both POPB (__syscall_handler) and
	// SAVE/RTN (syscall_dispatch), so -- unlike scheduler_probe.s --
	// it needs 040/042 initialized, and needs spval >= 0400 octal or
	// POPB spuriously stack-underflow-faults (trap_probe.s's own
	// already-documented finding, reused here). ----
	ELDA 0, spval
	STA 0, 040
	ELDA 0, sl
	STA 0, 042

	// ---- Load map A (Map[1]): page0 identity, page2 -> phys 0150 ----
	ELDA 0, zero
	DOA 0, MAP
	ELDA 0, zero
	ELDA 1, two
	ELEF 2, ptesA
	LMP

	// ---- Load map B (Map[2]): page0 identity, page2 -> phys 0044 ----
	ELDA 0, selB
	DOA 0, MAP
	ELDA 0, zero
	ELDA 1, two
	ELEF 2, ptesB
	LMP

	// ---- Install the syscall handler at location 2 ----
	ELDA 0, handleraddr
	STA 0, 2

	// ---- Arm the PIT for realism (real self-arming timer present in
	// this probe's environment, not relied on for the deterministic
	// race test -- see header) ----
	ELDA 0, pit_init
	DOA 0, PIT
	NIOS PIT

	JMP resume_A

// ============================================================
// Shared user-mode process body -- identity-mapped page 0, same for
// both processes (PROCESS_NOTES.md section 4). Modified from
// scheduler_probe.s's own user_loop: word0 (04200) is repurposed to
// record the SYSCALL's own return value instead of a constant id
// marker, because AC0 is legitimately clobbered by every SYC/POPB
// round trip (the return value comes back in AC0) -- the id-marker
// invariant scheduler_probe.s/PROCESS_NOTES.md already verified is not
// re-tested here (nothing about adding a syscall changes that
// mechanism); words 1-3 (running sum/step/iteration count) keep their
// original meaning and invariant unchanged, still a live corruption
// detector for AC1-AC3.
// ============================================================
user_loop:
	INC 3, 3		// AC3 (iteration count) += 1
	ADD 2, 1		// AC1 (running sum) += AC2 (fixed step)
	ESTA 1, 04201, 0	// word1: running sum
	ESTA 2, 04202, 0	// word2: step (must stay constant)
	ESTA 3, 04203, 0	// word3: iteration count

	// ---- the syscall under test. AC1 (running sum) and AC2 (step)
	// are passed through LIVE, unclobbered by design -- see this
	// file's header on why that's correct and sufficient, matching
	// TRAP_NOTES.md's own "AC2/AC3 pass through automatically" finding
	// applied here for the first time to a genuine call site. ----
	ELDA 0, syscall_reason
	SYC 1, 1
	ESTA 0, 04200, 0	// word0: this syscall's own return value

	JMP user_loop

// ============================================================
// Timer interrupt handler -- COPIED VERBATIM from
// examples/scheduler_probe.s (same file, same logic, not modified in
// any way for this probe -- see this file's own header). Only label
// names for shared data (save0, cur_mapstat, etc.) are unchanged so
// the comparison to the original stays exact.
// ============================================================
INTHANDLER:
	ESTA 0, save0

	DIA 0, MAP
	ESTA 0, cur_mapstat

	MOV# 0, 0, SZC
	JMP have_carry
	JMP no_carry
have_carry:
	ELDA 0, bit15mask
	JMP carry_captured
no_carry:
	ELDA 0, zero
carry_captured:
	ESTA 0, save_carrybit

	ESTA 1, save1
	ESTA 2, save2
	ESTA 3, save3

	LDA 0, 0
	ELDA 1, save_carrybit
	ADD 1, 0
	ESTA 0, save_pc

	DIB 1, 077
	ESTA 1, last_devcode

	ELDA 0, cur_mapstat
	ELDA 1, five
	SUB# 1, 0, SZR
	JMP was_A
	JMP was_B

was_A:
	ELDA 0, save0
	ESTA 0, tcbA_ac0
	ELDA 0, save1
	ESTA 0, tcbA_ac1
	ELDA 0, save2
	ESTA 0, tcbA_ac2
	ELDA 0, save3
	ESTA 0, tcbA_ac3
	ELDA 0, save_pc
	ESTA 0, tcbA_pc
	ELDA 0, cur_mapstat
	ESTA 0, tcbA_mapstat
	ELDA 0, switch_count
	INC 0, 0
	ESTA 0, switch_count
	JMP rearm_and_resume_B

was_B:
	ELDA 0, save0
	ESTA 0, tcbB_ac0
	ELDA 0, save1
	ESTA 0, tcbB_ac1
	ELDA 0, save2
	ESTA 0, tcbB_ac2
	ELDA 0, save3
	ESTA 0, tcbB_ac3
	ELDA 0, save_pc
	ESTA 0, tcbB_pc
	ELDA 0, cur_mapstat
	ESTA 0, tcbB_mapstat
	ELDA 0, switch_count
	INC 0, 0
	ESTA 0, switch_count
	JMP rearm_and_resume_A

rearm_and_resume_A:
	NIOC PIT
	NIOS PIT
	JMP resume_A

rearm_and_resume_B:
	NIOC PIT
	NIOS PIT
	JMP resume_B

resume_A:
	ELDA 0, tcbA_mapstat
	DOA 0, MAP

	ELDA 0, tcbA_pc
	ELDA 1, pcmask
	AND 1, 0
	ESTA 0, resume_target

	ELDA 0, tcbA_pc
	ELDA 1, bit15mask
	AND 1, 0
	MOV# 0, 0, SZR
	JMP setc1_A
	JMP setc0_A
setc0_A:
	MOVZ 0, 0
	JMP creset_A
setc1_A:
	MOVO 0, 0
creset_A:
	ELDA 1, tcbA_ac1
	ELDA 2, tcbA_ac2
	ELDA 3, tcbA_ac3
	ELDA 0, tcbA_ac0
	NIOS 077
	EJMP @resume_target

resume_B:
	ELDA 0, tcbB_mapstat
	DOA 0, MAP

	ELDA 0, tcbB_pc
	ELDA 1, pcmask
	AND 1, 0
	ESTA 0, resume_target

	ELDA 0, tcbB_pc
	ELDA 1, bit15mask
	AND 1, 0
	MOV# 0, 0, SZR
	JMP setc1_B
	JMP setc0_B
setc0_B:
	MOVZ 0, 0
	JMP creset_B
setc1_B:
	MOVO 0, 0
creset_B:
	ELDA 1, tcbB_ac1
	ELDA 2, tcbB_ac2
	ELDA 3, tcbB_ac3
	ELDA 0, tcbB_ac0
	NIOS 077
	EJMP @resume_target

// ============================================================
// __syscall_handler -- COPIED VERBATIM from examples/syscall_entry.s,
// POST-FIX (see that file's header for the full derivation and
// SYSCALL_NOTES.md section 7 for the race this closes): `NIOC 077`
// (INTDS) as the handler's own first instruction masks ALL device
// interrupts for the handler's entire duration, and `NIOS 077` (INTEN)
// immediately before `POPB` lifts the mask, using the exact same "NIOS
// then one guaranteed-safe instruction" idiom this file's own
// resume_A/resume_B (copied from scheduler_probe.s, below) already use
// before their own EJMP. `dev MAP = 03` is declared once, in the data
// section below, shared with INTHANDLER's own use of MAP (dgasm errors
// on a duplicate `dev` declaration -- examples/mmpu.c's own header
// comment already found this).
// ============================================================
__syscall_handler:
	NIOC 077			// INTDS -- see examples/syscall_entry.s
	ESTA 0, __sysh_reason
	ESTA 1, __sysh_arg1
	ESTA 2, __sysh_arg2

	DIA 0, MAP
	ESTA 0, __sysh_mapstat_raw

	ELDA 0, __sysh_arg1
	ESTA 0, sysarg1
	ELDA 0, __sysh_arg2
	ESTA 0, sysarg2

	ELDA 0, __sysh_reason
	ISZ 040, 0
	STA 0, @040
	EJSR syscall_dispatch, 0
	DSZ 040, 0

	LDA 2, 040
	STA 0, -4, 2

	ELDA 0, __sysh_mapstat_raw
	ELDA 1, __sysh_mask_no_bit0
	AND 1, 0
	ADI 0, 1
	DOA 0, MAP
	NIOS 077			// INTEN -- see examples/syscall_entry.s
	POPB

// ============================================================
// syscall_dispatch -- hand-written stand-in for examples/syscall.c's
// real compiled function, matching its exact calling-convention shape
// (DEBUGGING_NOTES.md entry #11: SAVE/MOV 3,2 prologue, one pushed
// 16-bit argument at FP-5, STA 0,-4,2 + STA 1,-3,2 + RTN epilogue).
// Records the reason it was called with and how many times, returns
// the running call count so user_loop's own word0 write (04200) is a
// direct, live witness of "syscall_dispatch really ran, this many
// times so far" -- not just "SYC returned something."
// ============================================================
syscall_dispatch:
	SAVE 4
	MOV 3, 2
	ELDA 0, -5, 2
	ESTA 0, last_reason
	ELDA 0, syscall_count
	INC 0, 0
	ESTA 0, syscall_count
	// ---- probe-only kill switch: HALT once enough syscalls have run.
	// A `step N` command that reaches SIMH's own step-budget limit
	// ("Step expired") never flushes the debug trace file in this
	// environment (confirmed empirically -- even examples/trap_probe.s
	// itself produces zero trace lines under a `step` count too small
	// to reach its own HALT, though it traces fully once a run reaches
	// one; see SYSCALL_NOTES.md). Since this probe's user_loop is a
	// genuine infinite loop (matching scheduler_probe.s, which never
	// halts either), a real HALT has to be engineered in on purpose so
	// each experiment's SESSION-FINAL step call actually flushes the
	// full accumulated trace, covering every earlier step/deposit in
	// the same session -- not a change to the syscall ABI itself, only
	// to this probe's own harness. ----
	ELDA 1, syscall_halt_at
	SUB# 1, 0, SZR
	JMP __sysd_no_halt
	HALT
__sysd_no_halt:
	STA 0, -4, 2
	STA 0, -3, 2
	RTN

	// NOTE: no explicit `org` jump here, unlike scheduler_probe.s's own
	// data section -- a real bug found and fixed while bringing this
	// probe up: this file's code (scheduler_probe.s's own INTHANDLER/
	// resume_A/resume_B PLUS __syscall_handler PLUS syscall_dispatch)
	// is longer than scheduler_probe.s's own code was, so
	// scheduler_probe.s's `org 0500` boundary -- safe for ITS code --
	// left too little room here: this probe's code actually ran past
	// address 0500 octal, so an explicit `org 0500` here would rewind
	// the address counter BACKWARD, placing data labels (and this
	// file's stack, spval=0400) on top of not-yet-executed handler
	// code. Confirmed the hard way: the very first SYC's own 5-word
	// return-block push (to addresses 0401-0405, immediately above
	// spval=0400) silently overwrote __syscall_handler's own
	// instructions at those addresses, corrupting it into "JMP 52" and
	// looping forever. Letting data simply continue sequentially after
	// syscall_dispatch's last instruction (no `org` here) avoids the
	// collision by construction -- confirmed by disassembly, not
	// merely by removing the symptom.
	dev MAP = 03
	dev PIT = 043

zero:
	dw 0
two:
	dw 2
five:
	dw 5
selB:
	dw 0400
spval:
	dw 03000		// comfortably above this file's own code AND
				// data (verified empirically -- see this
				// file's data-section header comment on the
				// org-0500 collision this replaces)
sl:
	dw 0177777
pcmask:
	dw 0077777
bit15mask:
	dw 0100000
pit_init:
	dw 0177770
handleraddr:
	dw __syscall_handler
syscall_reason:
	dw 42			// arbitrary, this probe's own single
				// "reason" -- not one of syscall.h's real
				// SYS_* numbers, since this probe's
				// syscall_dispatch stand-in doesn't dispatch
				// on it at all; kept distinct from 0-3 so a
				// trace is unambiguous about which
				// dispatcher (this probe's, vs the real one)
				// is in play if the two are ever compared
				// side by side.

ptesA:
	dw 0
	dw 04150
ptesB:
	dw 0
	dw 04044

// ---- scheduler scratch (verbatim names from scheduler_probe.s) ----
save0:
	dw 0
save1:
	dw 0
save2:
	dw 0
save3:
	dw 0
save_carrybit:
	dw 0
save_pc:
	dw 0
cur_mapstat:
	dw 0
last_devcode:
	dw 0
resume_target:
	dw 0
switch_count:
	dw 0

// ---- syscall handler scratch (verbatim names from syscall_entry.s) ----
__sysh_mask_no_bit0:
	dw 0177776
__sysh_reason:
	dw 0
__sysh_arg1:
	dw 0
__sysh_arg2:
	dw 0
__sysh_mapstat_raw:
	dw 0
sysarg1:
	dw 0
sysarg2:
	dw 0

// ---- syscall_dispatch's own scratch ----
last_reason:
	dw 0
syscall_count:
	dw 0
syscall_halt_at:
	dw 6

// ---- Process A's TCB ----
tcbA_ac0:
	dw 012345
tcbA_ac1:
	dw 2000
tcbA_ac2:
	dw 3
tcbA_ac3:
	dw 0
tcbA_pc:
	dw user_loop
tcbA_mapstat:
	dw 1

// ---- Process B's TCB ----
tcbB_ac0:
	dw 054321
tcbB_ac1:
	dw 6000
tcbB_ac2:
	dw 7
tcbB_ac3:
	dw 0
tcbB_pc:
	dw user_loop
tcbB_mapstat:
	dw 5
