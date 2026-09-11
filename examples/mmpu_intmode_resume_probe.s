// mmpu_intmode_resume_probe.s -- the actual open question a future
// preemptive scheduler design depends on: after a real interrupt lands
// mid-user-mode execution, does supervisor code have enough state to
// identify and correctly resume the SAME user map (A or B), or just
// "some user map was active" (mmpu_intmode_probe.s's finding)? Builds
// directly on mmpu_intmode_probe.s (same deterministic-interrupt-
// injection tooling -- read that file's header first) and
// mmpu_context_switch_probe.s (same two-map-loaded-at-once setup). See
// MMPU_NOTES.md's "Interrupt-safety of MMPU state" section for the full
// writeup this backs.
//
// Design: load both user maps (A -> phys page 0150 octal, B -> phys
// page 0044 octal, at logical page 2 -- exactly mmpu_context_switch_
// probe.s's own already-verified values, reused deliberately), enter
// user mode under map B specifically, get deterministically interrupted
// mid-execution (before the map-B write executes), then attempt TWO
// different resumes from the same interrupted point and see which one
// actually lands back on map B's physical target:
//
//   - "Naive" resume: reloads MapStat via DOA using only the documented
//     mechanism a from-scratch handler might default to (User Enable=1,
//     A/B bit left 0 -> Enable=1/map A) -- i.e. it does NOT use anything
//     DIA gave it.
//   - "Informed" resume: reloads MapStat via DOA using the *exact* value
//     the handler's own DIA read back at interrupt entry (saved to
//     mapstat_at_int, then replayed as literally `LDA 0,mapstat_at_int`
//     before the second DOA) -- no hardcoded secret constant standing in
//     for "the scheduler remembered," just hardware-sourced state played
//     straight back.
//
// THE FINDING (see the trace/memory evidence in the Run section below,
// and MMPU_NOTES.md for the full discussion): the "informed" resume
// lands correctly on map B every time; the "naive" one silently lands on
// map A instead -- no fault, no indication anything went wrong, just a
// process resumed under the wrong address space. This is possible to
// avoid, but only because of something not documented as part of
// MapIntMode/DIA's contract: eclipse_cpu.c's interrupt dispatch
// (~line 783-787) clears ONLY MapStat's bit 0 (User Enable) --
// `MapStat &= ~1;` -- leaving bit 13 (A/B select) and every other bit
// untouched. So an ordinary `DIA 0,MAP`, executed as literally the
// FIRST thing the handler does (before any other MAP-device I/O),
// returns a value that -- once bit 0 is restored via the documented
// MapIntMode-OR-in mechanism -- is bit-for-bit identical to the MapStat
// that was live immediately before the interrupt. That is a real,
// complete, correct resume descriptor, straight from hardware, with NO
// pre-existing software bookkeeping needed for THIS interrupt.
//
// What makes this fragile, not a guaranteed contract: (1) MapStat is a
// single live register, not a per-process save area or a stack -- the
// moment supervisor code issues ANY OTHER DOA to the MAP device (for
// this process's own housekeeping, for a second process's setup, or
// because a second interrupt arrives before the first is saved), that
// information is gone, exactly as this program's own "naive" resume
// demonstrates by clobbering it deliberately; (2) only bit 0 is
// documented as recoverable via MapIntMode/DIA at all -- bit 13's
// survival is a side effect of what ISN'T cleared, not something the
// manual or MapIntMode's own OR-in mechanism promises. A real OS
// therefore gets a narrow but usable guarantee: read-and-save Map Status
// as the literal first action of every interrupt handler, before
// touching the MAP device for any other reason, and ordinary software
// bookkeeping (a saved-MapStat word per process) takes it from there --
// the hardware does not maintain that across the rest of the handler's
// own execution, let alone across a real scheduler's context switch to
// a DIFFERENT process before eventually returning to this one.
//
// Assemble: dgasm -t eclipse_s140 -f simh -o mmpu_intmode_resume_probe.simh mmpu_intmode_resume_probe.s
// Run:
//   dep PC 50
//   step 17
//   dep PIT DONE 1
//   dep PIT INT 1
//   step 40
//
//   e 315       -- mapstat_at_int: 000005 octal, EXACTLY matching
//                  `enterB`'s own original DOA value (User Enable=1 +
//                  A/B=1) -- proof DIA's readback fully reconstructed
//                  the pre-interrupt MapStat, not just bit 0
//   e 320200    -- phys page 0150 octal (map A's target): 010341 octal
//                  = 4321 decimal = markerNaive -- the naive resume
//                  silently wrote into map A's physical page, even
//                  though the interrupted process was genuinely running
//                  under map B
//   e 110200    -- phys page 0044 octal (map B's REAL target): 021270
//                  octal = 8888 decimal = markerInformed -- the informed
//                  resume (using only the DIA-sourced value) correctly
//                  returned to map B and wrote to the right place
//   e 4200      -- plain logical (Usermap==0 now): 000000, confirming
//                  neither write ever touched unmapped memory
//
// The instruction trace (d debug 100003) independently confirms the
// same story via this project's own Usermap-label convention: the naive
// resume's ESTA is trace-prefixed "A" (Usermap==1); the informed
// resume's ESTA is trace-prefixed "B" (Usermap==2) -- the same physical
// divergence, seen a second, independent way.

	org 1
	var VECTOR = INTHANDLER

	org 050
_start:
	NIOS 077		// ION

	// ---- Load map A (Map[1]): page0 identity, page2 -> phys 0150 ----
	LDA 0, zero
	DOA 0, MAP		// Map Select = User A
	LDA 0, zero
	LDA 1, two
	ELEF 2, ptesA
	LMP

	// ---- Load map B (Map[2]): page0 identity, page2 -> phys 0044 ----
	LDA 0, selB
	DOA 0, MAP		// Map Select = User B
	LDA 0, zero
	LDA 1, two
	ELEF 2, ptesB
	LMP

	// ---- Enter user mode under B, run a known sequence, get interrupted ----
	LDA 0, enterB
	DOA 0, MAP		// MapStat: A/B=1, User Enable=1 -- Enable=2 (B)
	LDA 0, @iptr0		// TRIGGER: Usermap -> 2 (B)

	LDA 1, vX		// instr #1 under Usermap==2
	LDA 2, vY		// instr #2 -- interrupt lands at/after this
	ESTA 1, 04200, 0	// must NOT execute (would write through map
				// B before the interrupt point)
	HALT			// never reached

INTHANDLER:
	// reached in supervisor mode: Usermap forced 0, MapStat bit0 cleared
	DIB 1, 077		// ack
	DIA 0, MAP		// AC0 := MapStat with MapIntMode bit0 ORed in
	STA 0, mapstat_at_int	// save it BEFORE touching the MAP device
				// again -- this is the one read that still
				// has the pre-interrupt A/B state in it

	// ---- "Naive" resume: reconstructs only User Enable, not A/B ----
	LDA 0, naive_enter	// bit15 only -- A/B bit left 0 -> Enable=1 (A)
	DOA 0, MAP
	LDA 0, @iptrNaive	// TRIGGER: Usermap -> 1 (A) -- WRONG map for
				// the process that was actually interrupted
	LDA 1, markerNaive
	ESTA 1, 04200, 0	// lands at phys page 0150 (map A's target),
				// NOT map B's -- silently wrong, no fault
	NIOP MAP		// back to supervisor

	// ---- "Informed" resume: replay the EXACT value DIA gave us at
	// interrupt entry (saved to mapstat_at_int above), not a hardcoded
	// guess -- proves the bits needed for a correct resume genuinely
	// came from hardware readback, not from already knowing the answer
	LDA 0, mapstat_at_int
	DOA 0, MAP
	LDA 0, @iptrInformed	// TRIGGER: Usermap -> 2 (B) -- correct map
	LDA 2, markerInformed
	ESTA 2, 04200, 0	// lands at phys page 0044 (map B's real
				// target) -- correct resume
	NIOP MAP

	HALT

	org 0300
	dev MAP = 03
zero:
	dw 0
two:
	dw 2
selB:
	dw 0400
enterB:
	dw 5
naive_enter:
	dw 1
ptesA:
	dw 0
	dw 04150
ptesB:
	dw 0
	dw 04044
iptr0:
	dw landing0
landing0:
	dw 999
vX:
	dw 111
vY:
	dw 222
mapstat_at_int:
	dw 0
iptrNaive:
	dw landingNaive
landingNaive:
	dw 555
markerNaive:
	dw 4321
iptrInformed:
	dw landingInformed
landingInformed:
	dw 666
markerInformed:
	dw 8888
