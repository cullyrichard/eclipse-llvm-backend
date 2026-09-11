// mmpu_context_switch_probe.s -- hand-written, hand-verified proof that
// the Eclipse S/140's two real user maps (A and B) are genuinely
// independent: the same logical address, under each map in turn, lands
// on a completely different physical page -- the actual mechanism an
// OS would rely on to give two processes the same logical layout while
// keeping their physical memory separate. Builds directly on
// mmpu_usermode_probe.s's proven entry/exit mechanism (same identity-
// page-0 safety design, same indirect-reference trigger, same NIOP
// return path) -- read that file's header first. See MMPU_NOTES.md's
// "Phase 2: two-user-map context switch" section for the full writeup.
//
// Two separate, independent map-selection mechanisms, easy to conflate
// (both live in the same DOA-loaded MapStat word, but govern different
// things):
//   - "Map Select" (manual's bits 6-8, i.e. eclipse_cpu.c's
//     `(MapStat>>7)&07`, confirmed by LoadMap()'s own switch statement
//     matching this field's encoding exactly): which map context the
//     *next LMP* writes into. 000 = User A (Map[1]), 010 = User B
//     (Map[2]) -- confirmed against both the manual's own bit table
//     (Ch.5, DOA "Load Map Status") and LoadMap()'s switch names
//     ("0/1/2/3 = user maps A/C/B/D", see MMPU_NOTES.md's Phase 1
//     writeup) agreeing that index 2 (Map Select "010") is User B.
//   - "A/B" (manual's bit 13, confirmed directly in eclipse_cpu.c's
//     DEV_MAP DOA handler: `Enable = 1; if (MapStat & 04) Enable = 2;`
//     -- `04` octal is exactly bit 13 in this word, since DG's
//     MSB-first bit numbering puts bit 13 at LSB-first weight
//     2^(15-13) = 4): which map gets *activated* (Usermap=1 for A,
//     Usermap=2 for B) the next time User Enable (bit 15) fires.
// These are independent on purpose: this program loads *both* maps
// while still in supervisor mode (Map Select cycled A then B), then
// separately activates each one in turn (A/B cycled A then B) to
// demonstrate the switch, rather than needing to reload a map every
// time it's used.
//
// Safety design, same reasoning as mmpu_usermode_probe.s: both maps
// identity-map logical page 0 (where this program's own code/data up
// to org 0100 lives), so entering user mode under *either* map is
// invisible to normal execution -- only logical page 2 (deliberately
// non-identity in both maps, but pointing at two *different* physical
// pages) demonstrates the effect. Physical pages kept small and
// distinct, reusing known-good values from prior phases rather than
// picking fresh ones: map A's target (0150 octal) is exactly
// mmpu_usermode_probe.s's own verified physical page; map B's target
// (0044 octal) is exactly one of examples/mmpu_far_multi_test.c's
// verified physical pages. Both stay well under this SIMH eclipse
// binary's 128K-word (0400000 octal) console-examine ceiling
// (`awidth=17`, see mmpu_usermode_probe.s's header for the full
// citation) so both can be checked directly with console `e`.
//
// Entry/exit mechanism: identical to mmpu_usermode_probe.s (indirect
// LDA as the trigger, NIOP to return to supervisor) -- see that file's
// header for the citations. Done twice here, once per map, each with
// its own iptr/landing pair so the second trigger is a genuinely fresh
// indirect fetch, not reliant on any leftover state from the first.
//
// What this specifically demonstrates that no prior phase did: (1) a
// real switch between the *two* user maps the real S/140 has, not just
// a single map's entry/exit; (2) that switching maps changes only
// which map is *enabled*, not the loaded map data itself -- both maps
// stay loaded with their own distinct page-2 entries throughout; (3)
// that writing through map B does not disturb whatever was written
// through map A's own physical target -- checked directly by
// re-examining physical page 0150 octal *after* the map-B write, not
// just immediately after the map-A write.
//
// Assemble: dgasm -t eclipse_s140 -f simh -o mmpu_context_switch_probe.simh mmpu_context_switch_probe.s
// Run: dep PC 50, step ~60, then:
//   e 4200      -- logical page 2 offset 0200, Usermap=0 now (plain
//                  physical page 2): expect 0 (never touched by either
//                  map's write, which both went through translation)
//   e 320200    -- physical page 0150 octal offset 0200 (map A's
//                  target): expect 002322 octal (1234 decimal, the
//                  marker written while genuinely under user map A) --
//                  and still there, unmodified by the later map-B write
//   e 110200    -- physical page 0044 octal offset 0200 (map B's
//                  target): expect 021075 octal (8765 decimal, the
//                  marker written while genuinely under user map B)

	org 050

_start:
	// ---- Load map A (Map[1]): page0 identity, page2 -> phys 0150 ----
	LDA 0, zero
	DOA 0, MAP		// MapStat=0: Map Select=000 (User A), User
				// Enable=0 -- selects User A as LMP's target,
				// does NOT arm any mode transition
	LDA 0, zero
	LDA 1, two		// AC1 = 2 words to load
	ELEF 2, ptesA		// AC2 = address OF ptesA (not its value)
	LMP			// Map[1][0] = identity; Map[1][2] = phys 0150

	// ---- Load map B (Map[2]): page0 identity, page2 -> phys 0044 ----
	LDA 0, doa_selB
	DOA 0, MAP		// MapStat: Map Select=010 (User B), User
				// Enable=0 -- selects User B as LMP's target
	LDA 0, zero
	LDA 1, two
	ELEF 2, ptesB
	LMP			// Map[2][0] = identity; Map[2][2] = phys 0044

	// ---- Enter user mode under map A, write markerA through page 2 ----
	LDA 0, doa_enterA
	DOA 0, MAP		// MapStat: A/B=0 (User A), User Enable=1 --
				// arms the transition; Enable stays 1 (User A)
	LDA 0, @iptrA		// TRIGGER: indirect reference (still Usermap==0
				// for this fetch, since iptrA lives in identity
				// page 0) -- this is the fetch that flips
				// Usermap=1 per eclipse_cpu.c's effective()
	LDA 1, markerA
	ESTA 1, 04200, 0	// write AC1 (1234) to LOGICAL 04200 octal
				// (page 2, offset 0200) -- genuinely
				// translated via Map[1][2] to phys 0150 octal
	NIOP MAP		// pulsed while Usermap!=0: turns OFF user
				// mode (Usermap -> 0), back to supervisor

	// ---- Enter user mode under map B, write markerB through page 2 ----
	LDA 0, doa_enterB
	DOA 0, MAP		// MapStat: A/B=1 (User B), User Enable=1 --
				// Enable becomes 2 (User B) this time
	LDA 0, @iptrB		// TRIGGER: a fresh indirect reference (Usermap
				// is 0 going in, from NIOP above) -- flips
				// Usermap=2 per the same effective() path
	LDA 1, markerB
	ESTA 1, 04200, 0	// SAME logical address as the map-A write
				// above -- genuinely translated via
				// Map[2][2] this time, to phys 0044 octal,
				// NOT phys 0150 octal
	NIOP MAP		// back to supervisor

	HALT

	org 0200		// this program's code (two full map-load/
				// enter/write/return cycles, unlike
				// mmpu_usermode_probe.s's one) runs to
				// ~0104 octal -- 0100 collided with it (a
				// real bug, caught via instruction trace
				// showing "JMP 0" fetched from address 0100,
				// which is data (word value 0), not code);
				// 0200 leaves generous headroom
	dev MAP = 03

zero:
	dw 0
two:
	dw 2
doa_selB:
	dw 0400			// Map Select=010 (User B): bit 7 (DG
				// numbering) = LSB-weight 2^8 = 0400 octal.
				// Cross-checked two independent ways: (a) DG's
				// own bit table for "010" with bit6=0,bit7=1,
				// bit8=0; (b) eclipse_cpu.c's
				// `(MapStat>>7)&07` must equal 2 (LoadMap's
				// switch index for User B) when bit 8
				// (LSB-first) alone is set -- both agree.
doa_enterA:
	dw 1			// bit 15 (User Enable) only -- A/B bit (13)
				// left 0, so Enable defaults to 1 (User A).
				// Identical value to mmpu_usermode_probe.s's
				// own doaval, reused deliberately.
doa_enterB:
	dw 5			// bit 15 (User Enable) + bit 13 (A/B=1, User
				// B): 1 + 4 = 5 octal. Confirmed directly in
				// eclipse_cpu.c's DOA handler: `Enable = 1; if
				// (MapStat & 04) Enable = 2;` -- `04` octal is
				// exactly this bit.
ptesA:
	dw 0			// logical page 0 -> physical page 0 (identity)
	dw 04150		// logical page 2 -> physical page 0150 octal
				// (2<<10 | 0150 = 04000 | 0150 = 04150) --
				// same encoding mmpu_usermode_probe.s already
				// verified, reused exactly.
ptesB:
	dw 0			// logical page 0 -> physical page 0 (identity)
	dw 04044		// logical page 2 -> physical page 0044 octal
				// (2<<10 | 0044 = 04000 | 0044 = 04044)
iptrA:
	dw landingA
landingA:
	dw 111			// AC0 lands here after the map-A trigger LDA
markerA:
	dw 1234
iptrB:
	dw landingB
landingB:
	dw 222			// AC0 lands here after the map-B trigger LDA
markerB:
	dw 8765
