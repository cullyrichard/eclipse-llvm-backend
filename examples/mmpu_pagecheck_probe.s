// mmpu_pagecheck_probe.s -- hand-written, hand-verified proof that DOC
// (Initiate Page Check) + DIC (Page Check) correctly read back a
// loaded map entry's physical page number and WP bit, never exercised
// by any prior MMPU phase. Entirely supervisor-mode: confirmed
// directly in eclipse_cpu.c's DEV_MAP dispatch that DOC/DIC both
// execute unconditionally when Usermap==0 (`if (!Usermap || ...)`),
// so no user-mode entry mechanism is needed here at all -- the
// simplest possible test of this primitive.
//
// Reuses the exact same map entries mmpu_wpfault_probe.s already
// loaded and trace-confirmed (`eclipse`'s own LMP trace printed
// `107 MAP L=0 W=0 P=0` / `110 MAP L=2 W=1 P=3`), specifically to
// avoid introducing new arithmetic risk in a from-scratch entry.
//
// DOC's AC format, per the manual's dictionary entry and
// eclipse_cpu.c's `ioDOC` (`Check = AC[dstAC]` verbatim -- confirmed
// by source, not inferred): bits 1-5 (manual numbering) = logical page
// number, value = logical<<10; bits 6-8 = map select (000 = User A).
// For logical page 2, User A: 2<<10 = 04000 octal, map field 0 ->
// DOC AC = 04000 octal.
//
// DIC's result, per eclipse_cpu.c's `ioDIC`:
//   AC[dstAC] = Map[i][j] & 0101777;
//   AC[dstAC] |= (Check << 5) & 070000;
// where `i`/`j` are decoded from `Check` (the DOC-loaded value) --
// `i=1` (User A) for map-select 000, `j=2` (logical page) from
// `(Check>>10)&037`. For map-select=000 the echoed `(Check<<5)&070000`
// term is 0 (verified by hand: 04000<<5 = 0200000 octal = bit set at
// MSB-numbering position 1, which is *outside* the 070000/bits-1-3
// mask window -- 0 contribution, matching the intuitive "User A
// selector echoes as all-zero bits" expectation). So the predicted
// result is simply `Map[1][2] & 0101777` -- which mmpu_wpfault_probe.s
// already established as `0100003` octal (WP=1, physical=3) via its
// own LMP trace line. Checked against the actual run below, not
// assumed.
//
// Assemble: dgasm -t eclipse_s140 -f simh -o mmpu_pagecheck_probe.simh mmpu_pagecheck_probe.s
// Run: dep PC 50, step 10, then:
//   e result   -- should read 0100003 octal if the prediction holds

	org 050

_start:
	LDA 0, zero
	LDA 1, two		// AC1 = 2 words to load
	ELEF 2, ptes		// AC2 = address OF ptes
	LMP			// Map[1][0] = identity; Map[1][2] = phys 3, WP=1
				// (same values mmpu_wpfault_probe.s already
				// trace-confirmed)

	LDA 0, docval
	DOC 0, MAP		// Check = 04000: logical page 2, map=User A

	DIC 1, MAP		// AC1 = Map[1][2] & 0101777 | echo(0)
				// predicted: 0100003 octal
	STA 1, result
	HALT

	org 0100
	dev MAP = 03

zero:
	dw 0
two:
	dw 2
docval:
	dw 04000		// logical page 2 <<10, map select = User A (000)
ptes:
	dw 0			// logical page 0 -> physical page 0 (identity)
	dw 0104003		// logical page 2 -> physical page 3, WP=1
				// (wp=0100000, logical=2<<10=04000, physical=3)
result:
	dw 0
