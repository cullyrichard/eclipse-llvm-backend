// mmpu_wpfault_probe.s -- hand-written, hand-verified proof that a real
// Eclipse S/140 MMPU write-protection fault (distinct from the validity
// fault mmpu_fault_probe.s already proves) correctly fires and reaches
// the supervisor-mode handler, with a falsifiable prediction checked
// against the trace/memory dump rather than just observed after the
// fact. Builds directly on mmpu_fault_probe.s's fault-dispatch
// mechanism -- read that file's header first.
//
// Write protection's exact trigger, confirmed directly in
// eclipse_cpu.c's PutMap (case 1, User A), not inferred from the
// manual alone:
//   if (((Map[1][page] & 0100000) && (MapStat & 020)) ||
//       Map[1][page] == INVALID)
//       Fault = 010000;                    /* Write Protect Fault */
// Two independent conditions can set this, but this test isolates the
// first: bit0 of the *page's own* stored map word (0100000) AND
// MapStat's WP-enable bit (020 octal, the manual's "bit 11") both set,
// on an otherwise-VALID (non-INVALID-sentinel) page. This is different
// from mmpu_fault_probe.s's validity fault, which needs neither of
// those -- an INVALID page faults on any access, mapped or not,
// regardless of MapStat's WP-enable bit.
//
// Per the manual's own description ("Write Protection", Ch. 2 p.2-30):
// "When the user map is loaded, its address space is automatically
// write protected. Write protection can be enabled or disabled by the
// supervisor" -- matches the source exactly: bit0=1 marks a page as
// participating in write protection at all (every ordinary user-map
// page per the manual's own LMP note: "[bit 0] ... 1 for user maps"),
// and MapStat's WP-enable bit is the supervisor's global on/off switch
// for whether that protection is actually enforced right now. A page
// loaded with bit0=0 (like mmpu_fault_probe.s's/mmpu_usermode_probe.s's
// identity page 0) is *never* write-protected, regardless of MapStat --
// confirmed by the `&&` in the condition above, not assumed.
//
// Map entry values used here:
//   page 0 -> physical page 0, bit0=0 (dw 0): identity, NOT
//     write-protectable, used for this program's own code so it stays
//     safely executable/writable throughout, exactly like every prior
//     user-mode probe in this project.
//   page 2 -> physical page 3, bit0=1, VALID (not the all-1s INVALID
//     sentinel): 0100000 (WP participation bit) | 04000 (logical page
//     2<<10, consumed by LMP to pick the map slot, not itself stored)
//     | 3 (physical page) = 0104003 octal fed to LMP; what actually
//     lands in Map[1][2] after LoadMap's masking is 0100003.
//
// DOA value: User Enable (bit 15, manual numbering -> value 1) | WP
// Enable (bit 11, manual numbering -> value 020 octal) = 021 octal.
// (mmpu_fault_probe.s used plain `1` -- User Enable only, no WP Enable,
// which is exactly why it could only ever reach a validity fault.)
//
// Falsifiable prediction, worked out by hand from the fault-dispatch
// code (see mmpu_fault_probe.s's header/MMPU_NOTES.md for the full
// quote) *before* running this:
//   MapStat going in: 021 octal (User Enable=1, WP Enable=020).
//   `MapStat &= ~01` -> 020 octal (021 & ~1 = 020).
//   `if (Fault & 0100000) ...` -- Fault here is 010000 (Write Protect),
//     which does NOT have bit 0100000 set (that's the *validity* fault
//     code, a completely different bit) -- so this extra clear does
//     NOT fire, unlike the validity-fault case.
//   `MapStat |= Fault & 077777` -> 020 | 010000 = 010020 octal.
//   DIA masks with 0xFFFE (no-op here, already even) and ORs in
//   MapIntMode&1 (0, no interrupt occurred) -> DIA should read back
//   exactly 010020 octal. Verified against the actual run, not assumed
//   -- see MMPU_NOTES.md for whether this prediction held.
//
// Assemble: dgasm -t eclipse_s140 -f simh -o mmpu_wpfault_probe.simh mmpu_wpfault_probe.s
// Run: dep PC 50, step ~40, then:
//   e 65                -- mapstat_after: should be 010020 octal if the prediction holds
//   e 61 / e 62 / e 63 / e 64 / e 66  -- return block (AC0-AC3, PC)

	org 050

_start:
	LDA 0, spval
	STA 0, 040		// stack pointer = 060 octal
	LDA 0, sl
	STA 0, 042		// stack limit = 177777 (overflow protection off)

	LDA 0, handleraddr
	STA 0, 3		// loc 3 = pf_handler

	LDA 0, doaval
	DOA 0, MAP		// MapStat = 021: User Enable=1, WP Enable=1

	LDA 0, zero
	LDA 1, two		// AC1 = 2 words to load
	ELEF 2, ptes		// AC2 = address OF ptes
	LMP			// Map[1][0] = identity, not protectable
				// Map[1][2] = physical page 3, VALID, WP-participating

	LDA 0, @iptr		// TRIGGER: indirect reference flips Usermap=1
				// (page 0 identity, physically unchanged either way)

	// Genuinely in user mode from here. The write below targets
	// logical page 2 (Map[1][2] = physical page 3, bit0 set) while
	// MapStat's WP-enable bit is also set -- both write-fault
	// conditions are met simultaneously, deliberately.

	LDA 1, wval
	ESTA 1, 04200, 0	// WRITE to logical page 2, offset 0200.
				// PutMap sees Map[1][2]&0100000 (true) AND
				// MapStat&020 (true) -> Fault = 010000.
				// This instruction still completes; the
				// fault is consumed at the top of the next
				// loop iteration, before the HALT below.

	HALT			// never reached if the fault fires as expected

pf_handler:
	// Usermap already 0 (supervisor) by construction of the dispatch
	// code -- ordinary logical=physical addressing applies here.
	DIA 0, MAP		// AC0 = MapStat after the fault -- predicted 010020 octal
	STA 0, mapstat_after
	HALT

	org 0100
	dev MAP = 03

zero:
	dw 0
two:
	dw 2
doaval:
	dw 021			// User Enable | WP Enable
wval:
	dw 4321			// arbitrary marker -- should never actually land in memory
spval:
	dw 060
sl:
	dw 0177777
handleraddr:
	dw pf_handler
ptes:
	dw 0			// logical page 0 -> physical page 0, not write-protectable
	dw 0104003		// logical page 2 -> physical page 3, VALID, WP-participating
				// (wp=0100000, logical=2<<10=04000, physical=3
				// -> 0100000|04000|3 = 0104003; LMP consumes
				// the logical field to pick the map slot, only
				// wp+physical actually get stored)
iptr:
	dw landing
landing:
	dw 999
mapstat_after:
	dw 0
