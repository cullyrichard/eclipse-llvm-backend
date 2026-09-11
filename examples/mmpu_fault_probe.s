// mmpu_fault_probe.s -- hand-written, hand-verified proof that a real
// Eclipse S/140 MMPU validity protection fault, deliberately triggered
// from genuine user mode, correctly reaches a supervisor-mode handler
// installed at the documented location. Builds on mmpu_usermode_probe.s
// (same entry trigger, same identity-page-0 safety design) -- read that
// file's header first. See MMPU_NOTES.md's "Phase 2: page-fault
// handling" section for the full writeup this backs.
//
// Why validity, not write protection: both are real, always-reachable
// faults, but validity is simpler to set up -- per the manual
// ("Validity Protection", Ch. 2 p.2-30): "Validity protection is always
// enabled, so the supervisor's responsibility is limited to declaring
// the appropriate blocks of logical addresses invalid." Write
// protection additionally needs MapStat's WP-enable bit (bit 11 in the
// manual's numbering) set, one more moving part this test doesn't need.
// Confirmed directly in eclipse_cpu.c's GetMap (case 1, User A):
// `if (Map[1][page] == INVALID && !SingleCycle) Fault = 0100000;` --
// no separate enable check at all for validity on a read.
//
// The "declare invalid" encoding, per the manual's own NOTE under LMP's
// word format: "Declare a logical page invalid by setting the write
// protect bit to 1 and all of bits 6-15 to 1." For logical page 2:
// bit0(WP)=1 -> 0100000 octal; bits1-5(LOGICAL)=00010(=2) -> 04000
// octal (this field only selects *which* map slot LMP writes to --
// consumed, not stored, confirmed in Phase 1's LMP writeup); bits6-15
// (PHYSICAL)=1111111111 -> 01777 octal. Sum = 0105777 octal. After
// LoadMap's `Map[ctx][m] = w & MAPMASK` (MAPMASK = 0101777 = WP bit +
// all-1s physical field, the logical-selector bits masked away), this
// stores exactly `INVALID` (0101777, eclipse_cpu.c's own sentinel
// constant) into Map[1][2] -- confirmed by direct arithmetic, not
// assumed: 0105777 & 0101777 = 0101777.
//
// Fault dispatch mechanism, confirmed by reading eclipse_cpu.c's main
// loop directly (the `if (Fault) { ... }` block at the top, executed
// once per completed instruction, not mid-instruction):
//   Usermap = 0;                  -- current user map disabled (manual: yes)
//   MapStat &= ~01;                -- MMPU itself disabled
//   MapStat |= Fault & 077777;     -- fault code merged into MapStat
//   <push AC0-AC3, then PC, as a 5-word return block via PutMap>
//   PC = indirect(M[003]);         -- JMP to loc 3, read as RAW physical
//                                     memory (M[003], not GetMap(3)) --
//                                     confirms Ch.2's "unmapped logical
//                                     address space" for location 3
//                                     literally bypasses the map, not
//                                     just conceptually.
// Because Usermap is already 0 by the time the return block is pushed,
// that push goes through supervisor-mode PutMap (plain physical access
// for a low address) -- no special handling needed for it to land
// somewhere inspectable after the fault.
//
// Minimal stack setup: loc 40 (stack pointer) = 060 octal, loc 42
// (stack limit) = 177777 octal to disable overflow protection (both
// per Ch. 2's "Initializing the Stack Control Words" -- this test only
// cares that the 5-word push has somewhere valid to land, not about
// stack-overflow correctness).
//
// What this does NOT attempt, deliberately: recovering and resuming
// the faulted instruction. The manual itself (Ch. 2 p.2-30) says "A
// protection fault can occur at any point during the execution of an
// instruction. Therefore, the return address in the fifth word of the
// return block is not always correct" (only I/O protection faults get
// a guaranteed-correct return PC) -- so a generic "just RTN back" demo
// would misrepresent something DG's own manual flags as unreliable.
// This test stops at "fault occurs, handler receives control with
// correct, verifiable state" -- see MMPU_NOTES.md for the exact
// evidence and what was found empirically about MapStat's resulting
// bit pattern (checked directly, not derived from source alone -- see
// that file for why the derivation alone was not trusted here).
//
// Assemble: dgasm -t eclipse_s140 -f simh -o mmpu_fault_probe.simh mmpu_fault_probe.s
// Run: dep PC 50, step ~30, then:
//   e 41       -- frame pointer slot: AC1 from the return block (should be 2, the AC1 value at fault time)
//   e 44       -- AC0 will show pushed values starting at stack ptr+1; see MMPU_NOTES.md for exact addresses
//   e mapstat_after (resolve via listing, or just trust the trace)

	org 050

_start:
	LDA 0, spval
	STA 0, 040		// loc 40 = stack pointer = 060 octal
	LDA 0, sl
	STA 0, 042		// loc 42 = stack limit = 177777 (overflow protection off)

	LDA 0, handleraddr
	STA 0, 3		// loc 3 = pf_handler (direct target, bit0=0: not indirect)

	LDA 0, doaval
	DOA 0, MAP		// MapStat=1: User Enable=1, Map-select=User A,
				// A/B=User A -- arms the transition

	LDA 0, zero
	LDA 1, two		// AC1 = 2 words to load
	ELEF 2, ptes		// AC2 = address OF ptes
	LMP			// Map[1][0] = identity (valid); Map[1][2] = INVALID

	LDA 0, @iptr		// TRIGGER: indirect reference flips Usermap=1
				// on this fetch (page 0 is identity-mapped,
				// so this is physically identical either way)

	// Genuinely in user mode from here. Page 0 stays identity, so
	// this code keeps executing normally. The next access
	// deliberately targets logical page 2, which Map[1][2] marks
	// INVALID -- this is the fault trigger itself.

	ELDA 1, 04200, 0	// logical page 2, offset 0200 -- absolute,
				// non-indirect. GetMap(04200) computes
				// page=2, sees Map[1][2]==INVALID, sets
				// Fault=0100000 (Validity). This instruction
				// still completes (AC1 gets a meaningless
				// value) -- the fault is consumed at the top
				// of the NEXT loop iteration, before this
				// HALT below is ever fetched.

	HALT			// never reached if the fault fires as expected

pf_handler:
	// Reached via PC = indirect(M[003]). Usermap is already 0
	// (supervisor, unmapped) by construction of the dispatch code
	// above -- ordinary logical=physical addressing applies here.
	DIA 0, MAP		// AC0 = MapStat, now carrying whatever fault
				// history bits this fault set -- see
				// MMPU_NOTES.md for the exact value found
	STA 0, mapstat_after
	HALT

	org 0100
	dev MAP = 03

zero:
	dw 0
two:
	dw 2
doaval:
	dw 1
spval:
	dw 060
sl:
	dw 0177777
handleraddr:
	dw pf_handler
ptes:
	dw 0			// logical page 0 -> physical page 0 (identity, valid)
	dw 0105777		// logical page 2 -> INVALID (WP=1, physical=all-1s)
iptr:
	dw landing
landing:
	dw 999
mapstat_after:
	dw 0
