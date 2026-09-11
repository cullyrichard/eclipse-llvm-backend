// mmpu_usermode_probe.s -- hand-written, hand-verified proof that the
// Eclipse S/140's MMPU (via SIMH's eclipse_cpu.c emulation) can run
// real, sustained USER MODE (Usermap genuinely nonzero, every
// instruction fetch and data reference translated -- not the
// single-cycle trick mmpu_probe.s/examples/mmpu.c use). See
// MMPU_NOTES.md for the full writeup this backs.
//
// Design, and why it's safe: entering real user mode means the CPU's
// own next-instruction fetch (`IR = GetMap(PC)` in eclipse_cpu.c's
// main loop) is translated too, unlike the single-cycle mechanism
// which only ever redirects one data access. To make that safe
// without needing every instruction validated individually, this
// program stays entirely within logical/physical page 0 (org 050,
// everything below org 0100 too) and loads an IDENTITY map entry for
// page 0 (logical page 0 -> physical page 0) -- so code/data here
// reads identically whether Usermap is 0 (supervisor) or 1 (User A);
// the transition itself is invisible to normal execution. A SECOND,
// deliberately non-identity map entry (logical page 2 -> physical
// page 0150 octal) is what actually demonstrates real, sustained
// translation: a write through logical page 2 while genuinely in user
// mode lands at physical page 0150 octal, not physical page 2 -- and
// stays there after returning to supervisor mode, where logical=
// physical addressing would show physical page 2 untouched.
//
// Physical page kept small deliberately: this SIMH eclipse binary's
// CPU device declares awidth=17 (see eclipse_cpu.c's cpu_dev struct),
// so the console `e`/`d` commands cannot address physical memory at or
// above 128K words (0400000 octal), regardless of MEMSIZE/MAXMEMSIZE
// -- confirmed empirically (bisected the exact boundary: `e 377777`
// works, `e 400000` doesn't, even after `set cpu 1024k`). A running
// program's own GetMap/PutMap during real execution isn't limited
// this way at all -- only the interactive console is. Physical page
// 0150 octal keeps this test's verification addresses under that
// ceiling so they can be checked directly by console examine, the
// same way mmpu_probe.s's physical page 050 octal did.
//
// Trigger: per the manual (`DOA` "Load Map Status", bit 15/User
// Enable -- see MMPU_NOTES.md's Phase-2-context section), the actual
// mode switch happens on "the first memory reference after the next
// indirect reference or return type instruction" once MapStat bit 0
// is set. Confirmed directly in eclipse_cpu.c's `effective()`: the
// indirect-chain loop does `MA = GetMap(...); if (MapStat & 1) {
// Usermap = Enable; Inhibit = 0; }` on every indirect fetch -- so a
// plain `LDA 0,@iptr` is a controllable, minimal trigger, no stack
// setup needed (unlike POPJ/RTN/etc, the other documented triggers).
//
// Return path: per the manual (`NIOP` "Map Single Cycle / Disable
// User Mode" -- "From user mode -- If LEF mode and I/O protection are
// disabled, this instruction turns off the MMPU"), confirmed in
// eclipse_cpu.c: pulsing DEV_MAP while Usermap != 0 does
// `MapStat &= 0177776; Usermap = 0; Inhibit = 0;` -- the same NIOP
// instruction mmpu_probe.s uses to arm single-cycle mapping from
// supervisor mode does the *opposite* (disarm, return to supervisor)
// when pulsed from genuine user mode. Confirmed important subtlety:
// page 31's Map31 special-case (see MMPU_NOTES.md's "Unmapped Mode"
// citation) only applies while Usermap==0 -- GetMap's case 1/2 (real
// user maps) route page 31 through the ordinary Map[ctx][31] entry
// like every other page, not Map31. This program never references
// page 31 at all, sidestepping the question rather than relying on it.
//
// Assemble: dgasm -t eclipse_s140 -f simh -o mmpu_usermode_probe.simh mmpu_usermode_probe.s
// Run: dep PC 50, step ~30, then:
//   e 4200      -- logical page 2 offset 0200 (Usermap=0 now, so this
//                  reads physical page 2 directly): expect 0 (untouched)
//   e 320200    -- physical page 0150 octal offset 0200 (0150<<10 |
//                  0200): expect 5678 decimal (the value ESTA wrote,
//                  through the translated logical->physical redirect,
//                  while genuinely in user mode)

	org 050

_start:
	LDA 0, doaval
	DOA 0, MAP		// MapStat=1: Map-select=User A (bits 6-8
				// all zero -> User A, for LMP below),
				// A/B=User A (bit 13=0, for the activation
				// below), User Enable=1 (bit 15/C-bit0=1) --
				// arms the transition but does NOT flip
				// Usermap yet; only the next indirect
				// reference or return-type instruction does

	LDA 0, zero
	LDA 1, two		// AC1 = 2 words to load
	ELEF 2, ptes		// AC2 = address OF ptes (not its value)
	LMP			// Map[1][0] = identity (phys page 0);
				// Map[1][2] = phys page 0150 octal

	LDA 0, @iptr		// TRIGGER: indirect reference. iptr's own
				// stored value is fetched first (still
				// Usermap==0, since iptr lives in identity
				// page 0 this is physically identical
				// either way), and it's *that* fetch which
				// flips Usermap=1 per eclipse_cpu.c's
				// effective(). The final data fetch (of
				// `landing`, also page 0/identity) then
				// executes genuinely translated -- proving
				// nothing broke by checking AC0 below.

	// From here on Usermap==1 for real: every instruction fetch AND
	// data reference goes through Map[1]. Page 0 stays identity, so
	// this code keeps running exactly as written; logical page 2 is
	// the deliberately-non-identity page that proves it.

	LDA 1, farval
	ESTA 1, 04200, 0	// write AC1 (5678) to LOGICAL address 04200
				// octal (page 2, offset 0200) -- a bare
				// literal, not a symbol: `var X = N` (tried
				// first) allocates real storage (equivalent
				// to `X: dw N`, confirmed by reading
				// dgasm-src/assembler.c's VARIABLE_NUMBER
				// case) rather than a compile-time-only
				// alias, so it encoded the *address* of that
				// word, not 04200 itself -- caught via
				// `d debug 100003` instruction tracing,
				// which showed `ESTA 1,110` (the storage
				// word's own address) instead of the
				// intended target. This resolves via
				// Map[1][2] to physical page 0150 octal
				// while genuinely in user mode.

	NIOP MAP		// pulsed while Usermap!=0: this is NIOP's
				// *other* behavior (see header comment) --
				// turns OFF user mode, Usermap->0, back to
				// supervisor
	HALT

	org 0100
	dev MAP = 03

zero:
	dw 0
two:
	dw 2
doaval:
	dw 1
ptes:
	dw 0			// logical page 0 -> physical page 0
				// (identity: wp=0, logical=0, physical=0)
	dw 04150		// logical page 2 -> physical page 0150
				// octal (wp=0, logical=2<<10=04000,
				// physical=0150 -> 04000|0150 = 04150)
iptr:
	dw landing		// plain (non-indirect-continuing) pointer
landing:
	dw 999			// AC0 should hold this after the LDA above
farval:
	dw 5678
