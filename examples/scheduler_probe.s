// scheduler_probe.s -- hand-written, hand-verified real preemptive
// round-robin scheduler between exactly 2 concurrent user-mode processes
// on the Eclipse S/140, one resident in user map A, one in user map B,
// switched by a REAL, self-arming timer interrupt (not cooperative
// yielding, not SCP-console interrupt injection). See PROCESS_NOTES.md
// for the full design writeup and trace evidence; see examples/proc.h
// for the TCB field layout this file implements. Builds directly on:
//   - MMPU_NOTES.md's "Phase 2: MapIntMode" section's own conclusion,
//     now put to actual use rather than just investigated: preempting
//     user-mode MMPU-active code via a real interrupt is safe IF AND
//     ONLY IF the handler's first action is DIA 0,MAP before any other
//     MAP-device access. Violate that ordering and the resume silently
//     lands on the wrong address space -- mmpu_intmode_resume_probe.s
//     already demonstrated exactly that failure mode side-by-side with
//     the correct one; this file's INTHANDLER follows the safe ordering
//     throughout.
//   - mmpu_context_switch_probe.s's two-independent-user-maps mechanism
//     (Map Select vs A/B-enable, both maps loaded once at boot).
//   - examples/isr_c_test.c's interrupt-handler discipline (manual
//     register save/restore around the handler body, NIOC-then-NIOS
//     restart idiom for a periodic device) and examples/pit_timer_probe.s's
//     finding that this idiom applies to the PIT (device 043) too, and
//     that PIT really does self-arm/keep ticking with no further SCP
//     involvement once started.
//
// ============================================================
// 1. THE TIMER: real, self-arming, periodic -- not SCP-injected
// ============================================================
// examples/pit_timer_probe.s (committed separately, read its header for
// the full derivation from eclipse_cpu.c's pit()/pit_svc()) establishes
// this empirically, not just from source: `DOA ac,PIT` loads the 16-bit
// countdown starting value, a single `NIOS PIT` (pulse S, "start") arms
// it, and `pit_svc` -- the real tick handler, wall-clock-calibrated via
// sim_rtcn_calb like every timed SIMH device -- unconditionally
// reschedules ITSELF every tick with no software involvement, only
// actually raising a CPU interrupt once every 65536th tick (when
// `pit_counter` wraps past 0177777). Loading a small initial count (this
// file reuses pit_timer_probe.s's own 0177770 -- an 8-tick period) makes
// that wrap, and therefore a real interrupt, happen quickly and
// repeatedly with zero further `dep PIT ...` console commands after boot
// -- fully self-arming and periodic, confirmed by pit_timer_probe.s
// actually firing thousands of times unattended over tens of millions of
// real executed instructions.
//
// A real, load-bearing requirement pit_timer_probe.s found and this file
// reuses without re-deriving: the ONLY PIT operation that clears a
// pending Done/interrupt condition is `NIOC PIT` (pulse C) -- but
// eclipse_cpu.c's own iopC handler for PIT ALSO cancels the ticking unit
// (`sim_cancel(&pit_unit)`). So every tick this scheduler services must
// immediately re-issue `NIOS PIT` right after `NIOC PIT` to keep the
// timer genuinely periodic -- omit that and the SECOND tick never
// arrives. Same clear-then-restart idiom isr_c_test.c's handler already
// uses for the RTC/CLK device (NIOC 011 then NIOS 014), reused here for
// PIT specifically.
//
// Determinism, honestly not claimed: pit_timer_probe.s's own repeatability
// check (running the identical program twice) showed fire_count/last_ac1
// differing slightly between runs -- the same wall-clock-calibration
// non-determinism MMPU_NOTES.md's earlier MapIntMode investigation
// already found for the RTC/CLK device, now confirmed to extend to PIT's
// REAL self-arming path too (the SCP-deposit injection technique sidesteps
// exactly this, which is why it remains useful as a *testing* tool even
// though it is not how this scheduler arms its own timer). This file's
// own verification (PROCESS_NOTES.md) is therefore invariant-based --
// checking that whatever preemptions genuinely occurred left both
// processes' state internally consistent -- rather than pinned to an
// exact instruction count the way mmpu_intmode_resume_probe.s's
// SCP-deposit-based test could be.
//
// ============================================================
// 2. WHY ION IS NOT ENABLED UNTIL INSIDE resume_A/resume_B
// ============================================================
// Confirmed directly in nova_defs.h: INT_PENDING = INT_ION+INT_NO_ION_PENDING
// (bits 19+18), while every device interrupt bit (PIT included) lives at
// bit 17 or below (INT_DEV = (1<<INT_V_STK)-1). So `int_req > INT_PENDING`
// -- the main loop's only interrupt-dispatch gate -- is ALGEBRAICALLY
// FALSE whenever bit 19 (ION) is clear, no matter which device bits are
// pending: a pending PIT tick cannot be delivered while ION is off, full
// stop, not merely "unlikely." This lets _start arm the PIT (DOA+NIOS
// PIT) as early as convenient without any risk of a stray interrupt
// landing mid-setup (before either TCB reflects a real running process,
// which would hand the handler a meaningless cur_mapstat) -- ION itself
// is deliberately not turned on until the LAST instruction of
// resume_A/resume_B, immediately before the indirect JMP that both
// resumes user-mode execution AND (per the manual's own 1-instruction
// ION-enable delay, the same property every prior interrupt probe in
// this project already relies on) guarantees the earliest a real
// interrupt can land is inside user_loop itself, with a genuine, correct
// MapStat already live.
//
// ============================================================
// 3. THE TCB / SAVE-RESTORE DISCIPLINE
// ============================================================
// See examples/proc.h for the field layout. Two real subtleties beyond
// "copy 4 registers and a PC":
//
// - WHICH process was interrupted is determined from hardware, not
//   software bookkeeping, per the task's own explicit design (and
//   MMPU_NOTES.md's own recommendation): DIA's MapIntMode-informed
//   readback reproduces the pre-interrupt MapStat bit-for-bit (only bit
//   0 is ever cleared by the dispatch; bit 13, the A/B select, survives
//   untouched -- mmpu_intmode_resume_probe.s already proved this
//   directly). Since this scheduler only ever DOAs exactly two constants
//   (1 for A, 5 for B), a single equality test against 5 -- `SUB# 1,0,SZR`
//   with AC1=5 -- cleanly distinguishes them with no bitmasking needed.
//
// - CARRY is genuinely NOT saved automatically for this interrupt path.
//   SYC/fault dispatch (TRAP_NOTES.md, MMPU_NOTES.md's page-fault
//   section) fold C into the pushed return block's bit 15 IN HARDWARE --
//   but that is unique to those two vectors. A plain device interrupt
//   (location 1, what this scheduler uses) does only `M[0] = PC`, no
//   register push, no carry capture, confirmed directly in
//   eclipse_cpu.c's generic interrupt-dispatch block. This handler does
//   it by hand: capture C via a "#"(no-load) skip-test BEFORE any
//   non-"#" ALU instruction can disturb it (confirmed in source: the
//   no-load bit, dgasm's `#` suffix / encoding bit 0x8, skips the `C =
//   src&0200000` write entirely, not just the accumulator write -- this
//   is what makes "#"-suffixed instructions safe to use purely as
//   non-destructive tests throughout this project, this file included),
//   fold the captured bit into the saved PC the same way hardware does
//   for SYC/fault, and restore it later via `MOVZ 0,0` (forces C=0) /
//   `MOVO 0,0` (forces C=1) -- both confirmed by hand from the ALU
//   formula to leave the accumulator's OWN value completely unchanged
//   (MOV's "load"/"set" carry-in modes only ever affect the extra bit-16
//   carry slot, never bits 0-15 of a self-move), so this restore
//   sequence cannot corrupt AC0 even though it uses AC0 as its scratch
//   register.
//
// - Honest limitation, not smoothed over: the toy user_loop body below
//   does not have any computation that actually DEPENDS on carry-in from
//   a previous iteration (it uses plain ADD, not ADC-chained multi-word
//   arithmetic) -- so carry save/restore is implemented for TCB
//   completeness and structural correctness (matching the task's own
//   "AC0-AC3/PC/carry as needed for a full context" spec), but a broken
//   carry restore would NOT be visible in this probe's own verification
//   output the way a broken AC1/AC2 restore would be. Flagged here
//   rather than left implicit; see PROCESS_NOTES.md's "What this doesn't
//   prove" section.
//
// - dgasm addressing note: every data label used by this file lives well
//   past page zero (org 0500), so essentially every LDA/STA/JMP that
//   touches one uses the Extended forms (ELDA/ESTA/EJMP) -- plain
//   LDA/STA only reach 0-0377 octal (PMM_NOTES.md already hit this same
//   wall for the identical reason). The one exception is `LDA 0,0`,
//   reading the interrupted PC back from physical location 0 (Table
//   2.14's unmapped low page) -- a literal absolute address 0, safely
//   inside page zero, not a label.
//
// ============================================================
// 4. THE TWO "PROCESSES" AND WHY A WRONG RESTORE WOULD BE VISIBLE
// ============================================================
// Both processes run the SAME physical code (user_loop below) -- real,
// not a simplification: logical page 0 is identity-mapped in BOTH maps
// (mmpu_usermode_probe.s's established safety pattern), so the code
// itself is map-independent; everything that makes process A and
// process B behave differently lives entirely in their register state
// (AC0-AC3), which is exactly what a TCB is for. Each iteration:
//   AC3 (iteration count) += 1
//   AC1 (running sum)     += AC2 (a FIXED per-process step, loaded once
//                                  at TCB-init time and never reloaded
//                                  from a constant -- if a context switch
//                                  ever restored the WRONG TCB's AC2 into
//                                  this process, the running sum's own
//                                  growth rate would visibly change from
//                                  that point on, not just show one bad
//                                  value)
//   then AC0 (a constant per-process ID marker), AC1, AC2, and AC3 are
//   all written out through logical page 2 (translated to a distinct
//   physical page per map, exactly mmpu_context_switch_probe.s's own
//   already-verified targets: map A -> phys page 0150 octal, map B ->
//   phys page 0044 octal) -- so after however many real preemptions
//   actually occurred, reading back each process's 4 words from its own
//   physical target and checking the invariant
//       running_sum == seed + step * iteration_count
//   (worked out by hand from the seed/step constants below, exactly this
//   project's standing "falsifiable prediction, then confirm" discipline)
//   is a wrong-restore detector that does not depend on knowing the exact
//   number of preemptions in advance: ANY corruption of AC1, AC2, or AC3
//   across ANY switch -- including the classic "resumed the wrong
//   process" bug mmpu_intmode_resume_probe.s's own naive-resume path
//   demonstrated -- breaks this exact equality (mixing in the OTHER
//   process's step, in particular, breaks it almost certainly, since
//   stepA=3 and stepB=7 share no common small factor with each other's
//   seed offsets). The constant ID marker (AC0) is a second, even more
//   direct check: it must read back bit-for-bit identical on EVERY
//   inspection, forever, with no arithmetic involved at all.
//
// Process A: id=012345 octal, seed(AC1)=2000 decimal, step(AC2)=3 decimal
// Process B: id=054321 octal, seed(AC1)=6000 decimal, step(AC2)=7 decimal
//
// ============================================================
// Assemble: dgasm -t eclipse_s140 -f simh -o scheduler_probe.simh scheduler_probe.s
// Run and verification transcript: see PROCESS_NOTES.md.
// ============================================================

	org 1
	var VECTOR = INTHANDLER

	org 050
_start:
	// ---- Load map A (Map[1]): page0 identity, page2 -> phys 0150 ----
	ELDA 0, zero
	DOA 0, MAP		// Map Select = User A
	ELDA 0, zero
	ELDA 1, two
	ELEF 2, ptesA
	LMP

	// ---- Load map B (Map[2]): page0 identity, page2 -> phys 0044 ----
	ELDA 0, selB
	DOA 0, MAP		// Map Select = User B
	ELDA 0, zero
	ELDA 1, two
	ELEF 2, ptesB
	LMP

	// ---- Arm the PIT (real, self-arming, periodic -- see header) ----
	ELDA 0, pit_init
	DOA 0, PIT
	NIOS PIT

	// ---- Cold-boot straight into process A via the shared resume path.
	// ION is NOT enabled yet -- see header section 2 -- resume_A turns
	// it on as its own last act, immediately before the return jump.
	JMP resume_A

// ============================================================
// Shared user-mode process body. Identity-mapped in page 0 under BOTH
// maps, so this is genuinely the same code for either process -- see
// header section 4.
// ============================================================
user_loop:
	INC 3, 3		// AC3 (iteration count) += 1
	ADD 2, 1		// AC1 (running sum) += AC2 (fixed step)
	ESTA 0, 04200, 0	// word0: id marker (must stay constant)
	ESTA 1, 04201, 0	// word1: running sum
	ESTA 2, 04202, 0	// word2: step (must stay constant)
	ESTA 3, 04203, 0	// word3: iteration count
	JMP user_loop

// ============================================================
// Timer interrupt handler -- the scheduler itself.
// ============================================================
INTHANDLER:
	// ---- save true AC0 before it gets clobbered (STA never touches C) ----
	ESTA 0, save0

	// ---- MapIntMode contract: DIA MUST be the first MAP-device access,
	// before anything else touches the MAP device, or the pre-interrupt
	// A/B state is silently lost (MMPU_NOTES.md's own "Answer to the
	// scoping question") ----
	DIA 0, MAP
	ESTA 0, cur_mapstat

	// ---- capture carry BEFORE any non-"#" ALU instruction can disturb
	// it (STA/LDA/DIA/DOA/NIOx never touch C; only non-"#" ALU ops do) ----
	MOV# 0, 0, SZC		// "#": no accumulator/carry write, test only.
				// SZC skips next instr if carry is CLEAR.
	JMP have_carry
	JMP no_carry
have_carry:
	ELDA 0, bit15mask
	JMP carry_captured
no_carry:
	ELDA 0, zero
carry_captured:
	ESTA 0, save_carrybit

	// ---- save the rest of the raw interrupted registers ----
	ESTA 1, save1
	ESTA 2, save2
	ESTA 3, save3

	// ---- recover the interrupted PC (loc 0, raw/unmapped physical --
	// Table 2.14) and fold in the captured carry bit, same format the
	// hardware's own SYC/fault return block uses ----
	LDA 0, 0
	ELDA 1, save_carrybit
	ADD 1, 0		// AC0 := PC | carrybit (PC's own bit15 is
				// always 0, so ADD here is exactly OR)
	ESTA 0, save_pc

	// ---- device ack (only PIT is ever armed in this probe, but ack
	// properly anyway -- matches every prior interrupt probe) ----
	DIB 1, 077
	ESTA 1, last_devcode

	// ---- which process was interrupted? From hardware state (cur_mapstat),
	// not software bookkeeping -- see header section 3 ----
	ELDA 0, cur_mapstat
	ELDA 1, five
	SUB# 1, 0, SZR		// skip next instr if cur_mapstat == 5 (was B)
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
	JMP rearm_and_resume_B	// round robin: A was running -> resume B

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
	JMP rearm_and_resume_A	// round robin: B was running -> resume A

// ---- shared tail: ack+rearm the PIT (see header section 1 on why both
// NIOC and NIOS are required every time), then dispatch to the chosen
// resume routine ----
rearm_and_resume_A:
	NIOC PIT
	NIOS PIT
	JMP resume_A

rearm_and_resume_B:
	NIOC PIT
	NIOS PIT
	JMP resume_B

// ============================================================
// Resume routines. Each reloads its TCB's saved context and returns to
// it. Used both for the initial cold boot (_start jumps to resume_A
// directly) and for every subsequent post-interrupt resume -- the same
// routine works for both, since a cold-boot TCB is just a TCB whose
// fields were initialized statically instead of by a save. ION is
// enabled as literally the last instruction before the return jump --
// see header section 2.
// ============================================================
resume_A:
	ELDA 0, tcbA_mapstat
	DOA 0, MAP		// reprogram MapStat: User Enable=1, map A

	ELDA 0, tcbA_pc
	ELDA 1, pcmask
	AND 1, 0		// AC0 := pure resume PC (carry bit stripped)
	ESTA 0, resume_target

	ELDA 0, tcbA_pc
	ELDA 1, bit15mask
	AND 1, 0		// AC0 := isolated carry bit (0 or 0100000)
	MOV# 0, 0, SZR		// skip next instr if AC0 == 0 (no carry)
	JMP setc1_A
	JMP setc0_A
setc0_A:
	MOVZ 0, 0		// force C=0; AC0's value is unaffected
	JMP creset_A
setc1_A:
	MOVO 0, 0		// force C=1; AC0's value is unaffected
creset_A:
	ELDA 1, tcbA_ac1
	ELDA 2, tcbA_ac2
	ELDA 3, tcbA_ac3
	ELDA 0, tcbA_ac0	// true AC0 reloaded LAST (LDA never touches C)
	NIOS 077		// ION -- 1-instr delay covers the JMP below
	EJMP @resume_target	// TRIGGER: indirect ref flips Usermap per the
				// DOA above, lands exactly at the saved PC

resume_B:
	ELDA 0, tcbB_mapstat
	DOA 0, MAP		// reprogram MapStat: User Enable=1, map B

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

	org 0500
	dev MAP = 03
	dev PIT = 043

zero:
	dw 0
two:
	dw 2
five:
	dw 5
selB:
	dw 0400			// Map Select = User B (see
				// mmpu_context_switch_probe.s's derivation)
pcmask:
	dw 0077777		// strips bit15 (the carry-fold marker)
bit15mask:
	dw 0100000
pit_init:
	dw 0177770		// wraps (fires) after 8 ticks -- reused
				// unchanged from pit_timer_probe.s

ptesA:
	dw 0			// logical page 0 -> physical page 0 (identity)
	dw 04150		// logical page 2 -> physical page 0150 octal
ptesB:
	dw 0			// logical page 0 -> physical page 0 (identity)
	dw 04044		// logical page 2 -> physical page 0044 octal

// ---- interrupt-entry scratch (raw, not-yet-attributed-to-a-process) ----
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

// ---- Process A's TCB (examples/proc.h layout) -- initial values are
// this process's cold-boot state: seed 2000, step 3, iteration 0, entry
// at user_loop, map A ----
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
	dw 1			// TCB_MAPSTAT_A

// ---- Process B's TCB -- seed 6000, step 7, iteration 0, same entry
// point (shared code, see header section 4), map B ----
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
	dw 5			// TCB_MAPSTAT_B
