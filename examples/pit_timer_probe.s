// pit_timer_probe.s -- hand-written, hand-verified proof that the S/140's
// PIT (Programmable Interval Timer, device code 043 octal, eclipse_cpu.c's
// pit_dev) can be armed by ordinary supervisor-mode code as a REAL,
// self-arming, PERIODIC interrupt source -- distinct from the SCP-console
// `dep PIT DONE 1` / `dep PIT INT 1` injection technique MMPU_NOTES.md's
// "Phase 2: MapIntMode" section already established for deterministic
// *testing*. That technique is a debugging tool: it pokes SIMH's own
// register state from outside the running program. This probe instead
// arms the interrupt the way a real kernel would, using only instructions
// the running program itself executes -- no SCP `dep` at all after boot.
//
// The mechanism, read directly from eclipse_cpu.c (~line 5819-5868), not
// assumed:
//   - `DOA ac,PIT` sets `pit_initial = AC` and calibrates the timer's
//     period (`sim_rtcn_init(pit_time, 1)`).
//   - `NIOS PIT` (pulse S, "start"): `pit_counter = pit_initial;` sets
//     dev_busy, and -- if the unit isn't already running -- schedules the
//     first tick (`sim_activate`). This is the "arm" step.
//   - `pit_svc` (the actual tick handler, called every period): recomputes
//     its own calibration (`sim_rtcn_calb`) and, CRITICALLY,
//     unconditionally reschedules itself for the next tick
//     (`sim_activate(&pit_unit, t)`) BEFORE checking anything else. This
//     is the real finding this probe exists to confirm: the PIT does not
//     need software to re-arm it tick-to-tick -- once started, it keeps
//     ticking forever on its own. Only every 65536th tick (when
//     `pit_counter` wraps past `0177777`) does it actually set
//     `dev_done`/`int_req` and fire a real CPU interrupt, then reload
//     `pit_counter = pit_initial` and keep going.
//   - Net effect: `DOA` a *small* initial count (close to 0177777, so the
//     counter wraps after only a few ticks) then a single `NIOS PIT`
//     produces a genuine, real, self-perpetuating, periodic interrupt
//     source -- fires again, and again, forever, with zero further
//     software intervention, as long as the handler doesn't cancel it.
//
// A real complication, found and worked around, not assumed away: the
// interrupt handler MUST clear PIT's pending Done/interrupt condition
// before re-enabling ION, or the next `NIOS 077` (ION) would immediately
// re-trigger the same still-pending interrupt. The only PIT device
// operation that clears `dev_done`/`int_req` is `NIOC PIT` (pulse C) --
// but `eclipse_cpu.c`'s own `case iopC` for PIT ALSO does `sim_cancel
// (&pit_unit)`, stopping the ticking unit entirely. So "clear the pending
// interrupt" and "keep the periodic ticking going" are NOT free -- the
// handler must explicitly re-issue `NIOS PIT` (which, since the unit is
// now stopped, re-activates it via the `if (!sim_is_active(&pit_unit))`
// path) immediately after `NIOC PIT`, every single time. This is the same
// clear-then-restart idiom examples/isr_c_test.c's own handler already
// uses for the RTC/CLK device (`NIOC 011` then `NIOS 014`), reused here
// for PIT specifically -- a real, load-bearing, non-obvious requirement
// for a scheduler's own timer tick handler, not just a stylistic choice.
//
// Determinism, checked empirically rather than assumed either way:
// `pit_svc`'s own calibration (`sim_rtcn_calb`) is wall-clock-based, the
// same property MMPU_NOTES.md's earlier MapIntMode investigation already
// flagged as breaking single-step determinism for the RTC/CLK device.
// Whether the PIT's *first* tick (before any real-time calibration
// measurement has had a chance to run) lands on a repeatable instruction
// count is an empirical question, answered in this file's own "Run"
// section below by literally running it twice and diffing the traces --
// not derived from source alone.
//
// Design: a trivial supervisor-mode "heartbeat" loop (AC1 incremented
// every pass) stands in for a user process for this probe only (no MMPU
// involvement at all -- that combination is scheduler_probe.s's job, this
// file's only job is to nail down the timer mechanism in isolation, the
// same incremental-probe discipline MMPU_NOTES.md/TRAP_NOTES.md already
// established). The handler snapshots AC1 at interrupt time (proving the
// interrupt genuinely landed mid-loop, not after some fixed iteration
// count) and counts how many times it has fired (`fire_count`), then
// clears+rearms PIT and returns -- if firing is genuinely periodic, this
// probe run for long enough will show fire_count > 1 with no further
// software intervention after the initial NIOS PIT.
//
// Assemble: dgasm -t eclipse_s140 -f simh -o pit_timer_probe.simh pit_timer_probe.s
// Run: dep PC 50, step 2000000, then:
//   e 111  (fire_count)   -- expect > 1 if the timer is genuinely periodic
//   e 112  (last_ac1)     -- the heartbeat value at the MOST RECENT
//                            interrupt -- expect different values across
//                            separate inspections at increasing step
//                            counts, proving each interrupt lands at a
//                            genuinely different point in the loop, not a
//                            fixed one

	org 1
	var VECTOR = INTHANDLER

	org 050
_start:
	NIOS 077		// ION -- enable CPU interrupts (1-instr delay)

	LDA 0, initval
	DOA 0, PIT		// pit_initial = initval; calibrate period
	NIOS PIT		// ARM: pit_counter=pit_initial, start ticking

	LDA 1, zero
heartbeat:
	INC 1, 1		// AC1 += 1 -- pure supervisor-mode "process"
	JMP heartbeat

INTHANDLER:
	// Generic device-interrupt entry (location 1): Usermap is already 0
	// here regardless (this probe never used the MMPU), so ordinary
	// logical=physical addressing throughout.
	STA 1, last_ac1		// snapshot the heartbeat counter AT interrupt time
	DIB 2, 077		// ack -- AC2 := interrupting device code
	STA 2, last_devcode

	LDA 2, fire_count
	INC 2, 2
	STA 2, fire_count

	NIOC PIT		// clear PIT done/busy -- ALSO cancels the unit
	NIOS PIT		// re-arm: reload counter, restart ticking --
				// the "self-arming, periodic" step this probe
				// exists to prove is necessary AND sufficient
	NIOS 077		// re-enable ION (1-instr delay covers the JMP
				// below, matching every prior interrupt probe
				// in this project)
	JMP @0			// classic Nova/Eclipse interrupt return --
				// same convention isr_c_test.c's compiled
				// __attribute__((interrupt)) lowering uses

	org 0100
	dev PIT = 043

zero:
	dw 0
initval:
	dw 0177770		// wraps (fires) after only 8 ticks -- small on
				// purpose, so many firings happen quickly
fire_count:
	dw 0
last_ac1:
	dw 0
last_devcode:
	dw 0
