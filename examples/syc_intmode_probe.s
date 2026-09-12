// syc_intmode_probe.s -- does a real Eclipse S/140 SYC (System Call) trap
// give a supervisor-mode syscall handler enough state to recover which
// user map (A or B) was active in the interrupted process, the same way
// mmpu_intmode_resume_probe.s already proved a real DEVICE INTERRUPT does?
// See TRAP_NOTES.md's "SYC and MapIntMode" section for the full writeup;
// this file's own comments cover only what's specific to the probe.
//
// THE GAP THIS CLOSES (found during PROCESS_NOTES.md's scheduler work,
// section 8's closing note): eclipse_cpu.c's generic interrupt dispatch
// does `MapIntMode = MapStat;` BEFORE clearing MapStat's bit 0 -- that's
// the save half of the MapIntMode mechanism mmpu_intmode_resume_probe.s
// already verified a handler can read back via DIA. SYC's own dispatch
// (eclipse_cpu.c ~line 1798-1802: `DisMap = Usermap; Usermap = 0;
// MapStat &= ~1;`) does NOT do that save -- MapIntMode is left at
// whatever it held from the last real interrupt (usually stale or 0).
// TRAP_NOTES.md/PROCESS_NOTES.md flagged this as an open, previously-
// unverified gap; this probe is the actual test.
//
// THE FINDING, verified below: SYC's dispatch clears ONLY MapStat's bit
// 0 (`MapStat &= ~1`) -- bit-for-bit identical in shape to what the
// generic interrupt path does (`MapStat &= ~1` there too). Bit 13 (A/B
// select, `eclipse_cpu.c`'s `04` octal mask on MapStat -- mmpu_context_
// switch_probe.s's own citation) is therefore left completely untouched
// by SYC, exactly as it is by a real interrupt -- it was NEVER routed
// through MapIntMode at all, for either vector; MapIntMode's OR-in
// mechanism only ever covers bit 0. So `DIA 0,MAP`, executed as the
// SYC handler's very first action (before any other MAP-device I/O),
// returns bits 1-15 that are bit-for-bit the pre-trap MapStat -- the
// A/B identity is fully recoverable for SYC too, via the *same*
// "untouched bits" side channel mmpu_intmode_resume_probe.s already
// found for interrupts, NOT via MapIntMode (which SYC never populates).
//
// THE ONE REAL DIFFERENCE FROM THE INTERRUPT CASE: bit 0 itself. A real
// interrupt handler can trust DIA's bit 0 (MapIntMode's OR-in covers it
// correctly, mmpu_intmode_resume_probe.s already proved this). A SYC
// handler CANNOT -- MapIntMode's bit 0 is stale/unrelated to this trap,
// since SYC never sets MapIntMode. But this is not actually a gap for a
// syscall handler in practice: bit 0 (User Enable) is architecturally
// KNOWN to have been 1 immediately pre-trap, unconditionally, whenever
// DisMap > 0 (i.e. the trap came from real user mode, exactly the case
// a process running under a preemptive scheduler's timeslice hits) --
// SYC cannot even be reached with Usermap != 0 unless MapStat's bit 0
// was already 1 to get there. So the fix is: OR (reconstruct) bit 0 back
// in explicitly as a known constant, rather than trust DIA for it --
// verified below to be both NECESSARY (the "naive" call site 3, which
// skips this step, demonstrably fails) and SUFFICIENT (the "informed"
// call sites 1/2, which do it, demonstrably succeed for both A and B).
//
// A NEW, SYC-SPECIFIC FAILURE MODE, distinct from the interrupt case's
// "silently resumes under the wrong map": call site 3 shows that
// skipping the bit-0 reconstruction does not make SYC resume under the
// WRONG map -- it leaves MapStat's bit 0 OFF entirely (mapstat_raw's
// bit 0 is whatever stale MapIntMode bit happens to be, generally 0 in
// a fresh test with no prior real interrupt), so POPB's own
// `if (MapStat & 1) { Usermap = Enable; Inhibit = 0; }` (TRAP_NOTES.md's
// own citation) never re-arms Usermap at all. The process "returns" but
// silently keeps running as if it were supervisor code -- unmapped,
// unprotected, physical==logical addressing -- not a wrong address
// space, but NO address-space enforcement whatsoever. Demonstrated
// directly below: the naive path's marker write lands at plain physical
// address 04200, not translated through Map[1][2] to phys page 0150
// octal the way the informed path's otherwise-identical write does.
//
// Design: reuses mmpu_context_switch_probe.s's exact, already-verified
// map data (map A: logical page 2 -> phys page 0150 octal; map B:
// logical page 2 -> phys page 0044 octal) and trap_probe.s's exact
// SYC/POPB/identity-page-0 entry mechanism. THREE call sites, all
// through the SAME generic handler code (no per-call-site special
// casing in the handler -- the handler branches purely on a `reason`
// value the caller passes in AC0, the same convention trap_probe.s
// established):
//   1. Enter user mode under map A, SYC with reason=0 (informed
//      recovery) -- handler reads DIA, forces bit 0 on, DOAs, POPBs.
//      Expect: resumes under map A; ESTA through logical page 2 lands
//      at phys page 0150 octal.
//   2. Return to supervisor (NIOP), enter user mode under map B, SYC
//      with reason=0 again -- SAME handler code, now recovering B
//      instead of A, with no advance knowledge baked into the handler
//      about which map is "supposed" to be active. Expect: resumes
//      under map B; ESTA lands at phys page 0044 octal.
//   3. Return to supervisor, enter user mode under map A a third time,
//      SYC with reason=1 (naive -- deliberately skips the bit-0 fix).
//      Expect: MapStat ends up 0 after DOA, Usermap never reactivates,
//      the following ESTA runs unmapped and lands at plain physical
//      04200 -- NOT phys page 0150 -- demonstrating the failure mode
//      described above.
//
// Assemble: dgasm -t eclipse_s140 -f simh -o syc_intmode_probe.simh syc_intmode_probe.s
// Run: dep PC 50, step ~90, then:
//   e 320200    -- phys page 0150 octal, offset 0200 (map A's target):
//                  expect markerA (01234 octal) -- call site 1 (informed,
//                  map A) correctly recovered and resumed under A.
//   e 110200    -- phys page 0044 octal, offset 0200 (map B's target):
//                  expect markerB (05678 octal) -- call site 2 (informed,
//                  map B) correctly recovered and resumed under B, using
//                  the exact same handler code that just handled A.
//   e 4200      -- plain logical/physical page 2, offset 0200 (Usermap==0
//                  addressing): expect markerNaive (06060 octal) -- call
//                  site 3 (naive) never reactivated Usermap, so this
//                  write escaped translation entirely instead of landing
//                  at phys page 0150 like call site 1's otherwise-
//                  identical write did.

	org 050

_start:
	LDA 0, spval
	STA 0, 040		// stack pointer (0400 octal -- trap_probe.s's
				// own underflow-protection finding: must be
				// >= 0400 octal or POPB spuriously faults)
	LDA 0, sl
	STA 0, 042		// stack limit = 177777 (overflow protection off)

	LDA 0, handleraddr
	STA 0, 2		// loc 2 = SC HANDLER ADDRESS (direct, bit0=0)

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

	// ==== Call site 1: informed recovery, under map A ====
	LDA 0, enterA
	DOA 0, MAP		// User Enable=1, A/B=0 -> Enable=1 (A)
	LDA 0, @iptrA1		// TRIGGER -> Usermap=1

	LDA 0, reasonInformed
	LDA 1, tagA1		// argument, unused by this probe's own logic
				// beyond being captured -- parity with
				// trap_probe.s's calling convention
	SYC 1, 1		// NOT SYC 0,0 -- see trap_probe.s header
	LDA 1, markerA
	ESTA 1, 04200, 0	// via map A -> phys page 0150 octal
	NIOP MAP		// Usermap!=0 here -> back to supervisor

	// ==== Call site 2: informed recovery, under map B ====
	LDA 0, enterB
	DOA 0, MAP		// User Enable=1, A/B=1 -> Enable=2 (B)
	LDA 0, @iptrB1		// TRIGGER -> Usermap=2

	LDA 0, reasonInformed
	LDA 1, tagB1
	SYC 1, 1		// SAME handler code as call site 1
	LDA 1, markerB
	ESTA 1, 04200, 0	// via map B -> phys page 0044 octal
	NIOP MAP

	// ==== Call site 3: NAIVE (deliberately buggy) recovery, map A ====
	LDA 0, enterA
	DOA 0, MAP
	LDA 0, @iptrA2		// TRIGGER -> Usermap=1 (a fresh indirect
				// fetch -- not reusing call site 1's iptr)

	LDA 0, reasonNaive
	LDA 1, tagA3
	SYC 1, 1
	// If the naive path really leaves Usermap==0 after POPB, this ESTA
	// executes UNMAPPED (plain physical addressing), landing at
	// logical==physical 04200 -- not translated through Map[1][2].
	LDA 1, markerNaive
	ESTA 1, 04200, 0

	HALT

handler:
	// Reached via PC = indirect(GetMap(2)); Usermap is already 0 (SYC
	// forced it) -- ordinary physical addressing here, same as
	// trap_probe.s's handler.
	STA 0, reason_tmp	// capture reason before AC0 is reused
	STA 1, tag_tmp

	DIA 0, MAP		// THE critical ordering: first MAP-device
				// access since SYC fired. Bits 1-15 of the
				// result are exactly pre-trap MapStat's bits
				// 1-15 (SYC's dispatch clears only bit 0).
				// Bit 0 of the result is MapIntMode's bit 0,
				// which SYC never sets -- stale, not
				// trustworthy for this vector (see header).
	STA 0, mapstat_raw

	LDA 0, reason_tmp
	MOV# 0, 0, SZR		// skip next instruction if reason == 0
	JMP h_naive		// reason != 0 -> naive path
	JMP h_informed		// reason == 0 -> informed path

h_informed:
	// Reconstruct the full pre-trap MapStat: bits 1-15 come straight
	// from DIA (see above); bit 0 is forced on explicitly, because it
	// is architecturally KNOWN to have been 1 (SYC only reaches here
	// with DisMap>0 -- i.e. genuinely from user mode, which requires
	// MapStat bit 0 to already have been 1) -- not a guess, and not
	// something DIA can be trusted to supply for this vector.
	LDA 0, mapstat_raw
	LDA 1, mask_no_bit0	// 0177776 -- every bit except bit 0
	AND 1, 0		// AC0 := AC1 AND AC0 -- forces bit 0 to 0
				// first, so the next step can't corrupt bit
				// 1 via carry
	ADI 0, 1		// AC0 += 1 -- sets bit 0; safe, bit 0 was
				// just forced to 0 above (dgasm/eclipse_cpu.c
				// ADI operand order is ACC,IMM -- confirmed
				// directly in opcode.c's
				// encode_immediate_instruction, operand 0 is
				// the accumulator, operand 1 the immediate)
	STA 0, mapstat_resume
	JMP h_finish

h_naive:
	// Trusts DIA's bit 0 as-is -- the bug under test. In this probe
	// (no real interrupt has ever fired, so MapIntMode's bit 0 is
	// still its power-on/reset value) this reads back 0, so
	// mapstat_resume ends up with bit 0 OFF.
	LDA 0, mapstat_raw
	STA 0, mapstat_resume

h_finish:
	LDA 0, mapstat_resume
	DOA 0, MAP		// second (and last) MAP-device access before
				// POPB
	POPB			// pop AC0-AC3 + PC; reactivates Usermap only
				// if MapStat bit 0 (just DOA'd) is set

	org 0200		// past this program's own code (hand-counted
				// at ~69 words from org 050, comfortably
				// under 0200 octal/128 decimal) -- matches
				// mmpu_context_switch_probe.s's own margin
				// choice; must also stay under 256 decimal
				// (0377 octal) since every data reference
				// below uses plain page-zero-only LDA/STA,
				// not ELDA/ESTA (confirmed necessary --
				// see the regression note in TRAP_NOTES.md)
	dev MAP = 03

zero:
	dw 0
two:
	dw 2
selB:
	dw 0400			// Map Select = User B (mmpu_context_switch_
				// probe.s's own verified encoding)
enterA:
	dw 1			// User Enable=1, A/B=0 -> Enable=1 (User A)
enterB:
	dw 5			// User Enable=1, A/B=1 -> Enable=2 (User B)
mask_no_bit0:
	dw 0177776		// all bits except bit 0
reasonInformed:
	dw 0
reasonNaive:
	dw 1
spval:
	dw 0400
sl:
	dw 0177777
handleraddr:
	dw handler
ptesA:
	dw 0			// logical page 0 -> physical page 0 (identity)
	dw 04150		// logical page 2 -> physical page 0150 octal
ptesB:
	dw 0			// logical page 0 -> physical page 0 (identity)
	dw 04044		// logical page 2 -> physical page 0044 octal
iptrA1:
	dw landingA1
landingA1:
	dw 111
iptrB1:
	dw landingB1
landingB1:
	dw 222
iptrA2:
	dw landingA2
landingA2:
	dw 333
tagA1:
	dw 1001
tagB1:
	dw 2002
tagA3:
	dw 3003
markerA:
	dw 01234
markerB:
	dw 5678			// decimal (no leading zero) -- 05678 would be
				// an invalid octal literal (digit '8'); the
				// same "needs a leading zero, and only valid
				// octal digits" gotcha MMPU_NOTES.md/mktape.py
				// already documented, caught here by comparing
				// this run's actual output against the
				// intended value before trusting it
markerNaive:
	dw 06060
reason_tmp:
	dw 0
tag_tmp:
	dw 0
mapstat_raw:
	dw 0
mapstat_resume:
	dw 0
