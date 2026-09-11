// mmpu_intmode_probe.s -- hand-written, hand-verified proof that a real
// hardware interrupt landing mid-USER-MODE execution (Usermap != 0, via
// the MMPU) is correctly captured by MapIntMode, and that a supervisor
// handler can read that fact back with a single DIA. See MMPU_NOTES.md's
// "Interrupt-safety of MMPU state (MapIntMode)" section for the full
// writeup and the prior investigation this builds on.
//
// THE TOOLING PROBLEM THIS SOLVES FIRST: every interrupt source in this
// SIMH eclipse build (RTC/PIT's own pit_svc, TTI/TTO character
// completion) is wall-clock-calibrated (sim_rtcn_calb tunes the delay
// against the *host's* real clock), not instruction-count-deterministic
// -- the previous MapIntMode investigation stopped exactly here, because
// this project's evidentiary bar requires knowing, in advance and
// repeatably, which instruction an interrupt lands on.
//
// The fix does not touch the timed service routine at all: SIMH's SCP
// console can `deposit` directly into a device's own register set
// between `step` calls, and eclipse_cpu.c's PIT device exposes both
// `dev_done`'s and `int_req`'s own PIT bit as ordinary, non-REG_RO
// registers (`pit_reg[]`: `{ FLDATA (DONE, dev_done, INT_V_PIT) }`,
// `{ FLDATA (INT, int_req, INT_V_PIT) }`). `dep PIT DONE 1` + `dep PIT
// INT 1` sets exactly the same two bits pit_svc's own timed completion
// path would set (dev_done's PIT bit, then int_req recomputed from it)
// -- but does it the instant the SCP command runs, between two `step`
// calls, with zero dependency on wall-clock time. Run `step N` to reach
// an exact, chosen instruction boundary, deposit those two bits, then
// `step` again: the interrupt fires on the very next loop iteration's
// `if (int_req > INT_PENDING && !Inhibit)` check (eclipse_cpu.c ~line
// 783), landing precisely between instruction N and N+1 -- fully
// deterministic and scriptable, confirmed empirically below (and its
// own control case, a plain supervisor-mode program with no MMPU
// involvement, landed an interrupt exactly after a chosen instruction
// with byte-identical, repeatable results before this file was written).
//
// Trigger for CPU interrupts themselves: `NIOS 077` (device 077 = CPU
// control, S pulse) is the classic Nova/Eclipse ION instruction --
// confirmed directly in eclipse_cpu.c's DEV_CPU pulse handler: `case
// iopS: int_req = (int_req | INT_ION) & ~INT_NO_ION_PENDING;`. There is
// a real one-instruction delay before this takes effect (`int_req =
// int_req | INT_NO_ION_PENDING;` runs unconditionally after every
// instruction fetch, ~line 905) -- this program's setup sequence has
// several instructions between NIOS and the injected interrupt, so the
// delay is long past by the time it matters.
//
// MMPU entry: identical mechanism to mmpu_usermode_probe.s (DOA arms
// User Enable, the next indirect reference is what actually flips
// Usermap) -- read that file's header first if this is unfamiliar. Page
// 0 is identity-mapped in map A so code here runs identically whether
// Usermap is 0 or 1.
//
// What this specifically checks: eclipse_cpu.c's interrupt dispatch
// (~line 783) does `MapIntMode = MapStat;` (saves the *whole* register)
// then `Usermap = 0; MapStat &= ~1;` (forces supervisor mode) *before*
// the vector dispatch -- unconditionally, for any interrupt source, not
// just MAP-device-related ones. `DIA` (Read Map Status, ~line 5181) ORs
// `MapIntMode & 1` into the returned value's own bit 0 -- "was User
// Enable set at the moment this interrupt landed." This program forces
// a real interrupt into the middle of a genuine, sustained user-mode (A)
// instruction sequence and has the handler read that bit back, both for
// the "yes, active" case and (a second run, different step count) the
// "no, still supervisor" case -- a real contrast, not just one
// data point.
//
// Assemble: dgasm -t eclipse_s140 -f simh -o mmpu_intmode_probe.simh mmpu_intmode_probe.s
// Run (three separate scenarios, same binary, different injection point):
//
//   (1) Genuinely active user mode (Usermap==1 at injection):
//     dep PC 50, step 12, dep PIT DONE 1, dep PIT INT 1, step 4
//     -> e 212 (devcode): 000043 (DEV_PIT, confirms interrupt identity)
//     -> e 213 (mapstat_at_int): 000001 (User Enable WAS active)
//     -> e 320200 (phys page 0150, map A's target): 000000 (the ESTA
//        after the injection point never ran -- the interrupt genuinely
//        landed where planned, not "eventually")
//
//   (2) True supervisor, well before MMPU is even armed:
//     dep PC 50, step 5, dep PIT DONE 1, dep PIT INT 1, step 4
//     -> e 213 (mapstat_at_int): 000000 (User Enable was NOT active --
//        the negative control)
//
//   (3) Armed (DOA already set User Enable=1) but the mode-switch
//   trigger hasn't executed yet -- the theoretically ambiguous window:
//     dep PC 50, step 9, dep PIT DONE 1, dep PIT INT 1, step 5
//     -> e 213 (mapstat_at_int): 000001
//     This is NOT the ambiguous case it looks like: DOA setting User
//     Enable also sets `Inhibit = 2` (eclipse_cpu.c's ioDOA handler),
//     and the interrupt-check is gated on `!Inhibit` -- so the interrupt
//     is hardware-blocked from landing in that window at all. It can
//     only fire once Inhibit clears, which happens *inside* the trigger
//     instruction itself (`Usermap = Enable; Inhibit = 0;`) -- so by the
//     time an interrupt can land, Usermap has *already* flipped to 1.
//     Confirmed directly in the instruction trace: the interrupt fires
//     immediately after the trigger instruction (`LDA 0,@iptr`)
//     completes, never before it, across 9-then-5 step counts chosen
//     specifically to probe this boundary. There is no observable state
//     where MapStat's User-Enable bit is 1 but Usermap is still
//     genuinely 0 -- DIA's bit 0 is a clean signal.

	org 1
	var VECTOR = INTHANDLER

	org 050
_start:
	NIOS 077		// ION -- enable CPU interrupts (1-instr delay)

	LDA 0, zero
	DOA 0, MAP		// MapStat=0: Map Select=User A, User Enable=0
	LDA 0, zero
	LDA 1, two
	ELEF 2, ptes
	LMP			// Map[1][0]=identity, Map[1][2]=phys 0150

	LDA 0, doa_enter
	DOA 0, MAP		// MapStat: User Enable=1, A/B=0 (User A) -- arms
	LDA 0, @iptr		// TRIGGER: flips Usermap=1

	LDA 1, v1		// known sequence under Usermap==1, instr #1
	LDA 2, v2		// instr #2
	LDA 3, v3		// instr #3 -- interrupt should land AFTER this
	ESTA 1, 04200, 0	// instr #4 -- must NOT execute if interrupt
				// lands as intended
	HALT			// never reached if handler halts instead

INTHANDLER:
	// Reached in supervisor mode: hardware already forced Usermap=0 and
	// cleared MapStat's User-Enable bit before dispatch.
	DIB 1, 077		// ack: AC1 := interrupting device code
	STA 1, devcode
	DIA 0, MAP		// AC0 := MapStat with MapIntMode bit0 ORed in
	STA 0, mapstat_at_int
	HALT

	org 0200
	dev MAP = 03
zero:
	dw 0
two:
	dw 2
doa_enter:
	dw 1
ptes:
	dw 0			// logical page 0 -> physical page 0 (identity)
	dw 04150		// logical page 2 -> physical page 0150 octal
iptr:
	dw landing
landing:
	dw 999
v1:
	dw 111
v2:
	dw 222
v3:
	dw 333
devcode:
	dw 0
mapstat_at_int:
	dw 0
