// trap_probe.s -- hand-written, hand-verified proof of a real Eclipse S/140
// VOLUNTARY supervisor-mode trap: user-mode code deliberately executes an
// instruction that transfers control to a supervisor handler (as opposed
// to isr_c_test.c's INVOLUNTARY, device-triggered interrupt, or
// mmpu_fault_probe.s's INVOLUNTARY MMPU protection fault). This is the
// real S/140 hardware mechanism for exactly the syscall/SVC/TRAP role a
// Unix-like OS needs. See TRAP_NOTES.md for the full writeup and manual
// citations; this file's own comments cover only what's specific to the
// probe itself.
//
// The instruction: SYC (System Call, aliases SCL/SVC -- same opcode,
// Table 2.17), manual Ch. 4 p.4-72: "Pushes a return block and transfers
// control to the system call handler ... The program counter in the
// return block points to the instruction immediately following the
// System call instruction. After pushing the return block, the
// instruction executes a jump indirect to location 2, which contains the
// address of the system call handler." Location 2 (SC HANDLER ADDRESS,
// indirectable) is confirmed in the same reserved-locations table
// (Table 2.14) mmpu_fault_probe.s already used for location 3 (PF HANDLER
// ADDRESS) -- same mechanism family, different fixed vector.
//
// Confirmed directly in eclipse_cpu.c (~line 1798, `if ((IR & 0103777) ==
// 0103510)`), not just the manual prose:
//   DisMap = Usermap; Usermap = 0; MapStat &= ~1;      // leave user mode
//   i = (IR>>13)&3; j = (IR>>11)&3;                    // ACS, ACD fields
//   if (i != 0 || j != 0) {                            // NOT both AC0
//       push AC0,AC1,AC2,AC3,PC(+carry) -- 5 words, same order/format
//       mmpu_fault_probe.s's fault return block already used
//   }
//   PC = indirect(GetMap(2));
//   if (DisMap > 0) Inhibit = 3;   // 1-instruction interrupt inhibit,
//                                  // only when the trap came from real
//                                  // user mode -- matches the manual's
//                                  // "I/O interrupts cannot occur
//                                  // between the System call instruction
//                                  // and the handler's first instruction"
// This is why this probe deliberately uses `SYC 1,1` (ACS=ACD=AC1), not
// `SYC 0,0` -- the manual's own NOTE says AC0,AC0 is special-cased to
// skip the push entirely ("the instruction does not push a return block
// onto the stack"), confirmed by the exact same `i != 0 || j != 0` check
// in source. Any other ACS/ACD combination behaves identically -- neither
// register's *value* affects SYC's behavior, only whether both fields
// happen to select AC0. dgasm's own base encoding for SYC (opcode.c) is
// octal 0103510 with ACS/ACD fields zeroed -- computed by hand and
// confirmed to match eclipse_cpu.c's `0103510` comparison constant
// exactly, so both independent real implementations (assembler + CPU
// model) agree on the encoding.
//
// Calling convention this probe establishes and verifies (not itself
// hardware-enforced -- SYC's ACS/ACD fields carry no data, so any
// register convention is a software choice; this is the one a future
// syscall layer should use, chosen to match this codebase's existing
// AC0-is-return-value convention from DEBUGGING_NOTES.md entry 11):
//   AC0 = syscall number (input), then overwritten with a return value
//         (output) by the handler -- see the return-value fixup below.
//   AC1 = one argument (input only in this probe).
// Input works "for free": SYC doesn't touch the live AC0-AC3 registers at
// all (only copies them to the stack), so the handler reads them exactly
// as the user code left them, no different from isr_c_test.c reading
// DIB's result into AC0.
//
// Output is the non-obvious part, verified here rather than assumed:
// return-to-caller uses POPB ("Returns control from a System Call
// routine", manual p.4-68), and POPB restores AC0-AC3 by POPPING THE
// STACK, not from whatever the handler's live registers hold at POPB
// time (confirmed in eclipse_cpu.c ~line 1525: `AC[0] = GetMap(GetMap(040))`
// after walking back down the pushed block) -- so a handler that just does
// `LDA 0,newretval` before POPB accomplishes nothing; POPB overwrites AC0
// right back from the stack's original snapshot. This is the exact same
// hazard DEBUGGING_NOTES.md entry 11 already found and fixed for
// SAVE/RTN ("RTN unconditionally restores AC0/AC1 to their pre-call
// values -- an epilogue has to overwrite those two saved stack slots
// with the real return value"). The fix is identical here: the handler
// loads AC2 with the current stack pointer (location 040's contents,
// which is exactly the post-push SP -- confirmed by hand from the push
// arithmetic above: new_SP = old_SP+5, and the AC0 slot sits at
// old_SP+1 = new_SP-4) and stores the real return value to `-4,2`
// (AC2-relative addressing, the same addressing mode this backend's own
// compiled epilogues already use for this exact purpose).
//
// Two distinct trap "reasons" (a minimal syscall-number dispatch,
// generalizing isr_c_test.c's single hardwired handler into something
// that can tell requests apart): reason 0 and reason 1 reach genuinely
// different code paths (path_a/path_b below), each recording a distinct
// marker built from its own argument and returning a distinct,
// recognizable value (0125252 octal = 0xAAAA for reason 0, 0052525
// octal = 0x5555 for reason 1) -- so a wrong-path bug (e.g. reason 1
// accidentally running path_a's code) would show up as the wrong marker
// or return value, not just "something happened."
//
// Two distinct call sites (proving the return address is genuinely
// per-call, not some fixed/hardcoded resumption point): call site 1
// executes SYC with reason 0, call site 2 -- a completely different
// address -- executes SYC with reason 1. Each site's own next instruction
// (call1_land / call2_land) stores the returned AC0 to its own separate
// memory cell, so a return-address bug (e.g. always resuming at
// call1_land) is directly visible in the dump.
//
// Same identity-page-0 user-mode entry this project always uses
// (mmpu_usermode_probe.s's pattern): logical page 0 maps to physical
// page 0, so code here runs identically whether Usermap is 0 or 1 --
// this probe's own code is never at risk from the map, only the
// SYC/POPB mechanism itself is under test.
//
// Re-entering user mode after the trap: SYC's dispatch code above clears
// MapStat's bit 0 (`MapStat &= ~1`) unconditionally -- so by the time the
// handler is ready to return, MapStat's User Enable bit is OFF, and
// POPB's own `if (MapStat & 1) { Usermap = Enable; ... }` (eclipse_cpu.c
// ~line 1547) would do nothing unless something turns it back on first.
// This probe's handler re-issues the same DOA that originally enabled
// User Enable/map A before POPB, confirmed empirically below to be
// necessary and sufficient -- `Enable` itself (which map, A or B) is
// untouched by SYC, so this DOA does not need to repeat any LMP work.
//
// What this deliberately does NOT attempt: a trap taken while already in
// supervisor mode (DisMap == 0 case -- no Inhibit set, and nothing here
// exercises it), or nested traps (a SYC executed from inside this
// handler). See TRAP_NOTES.md's "What's open" section.
//
// A real bug found and fixed by this probe specifically, the same class
// this project keeps finding (see MMPU_NOTES.md's own running list):
// every prior stack-using probe (mmpu_fault_probe.s, mmpu_wpfault_probe.s)
// used `spval = 060` and never popped the stack back down, so they never
// exercised STACK UNDERFLOW PROTECTION. This probe is the first to
// actually pop (POPB), and the first run with spval=060 failed exactly
// there: POPB's own underflow check (eclipse_cpu.c ~line 1540, `t =
// GetMap(040); if (t < 0100000 && t < 0400) { ...fault... }`) fired
// because 060 octal (48 decimal) is below the manual's own documented
// threshold (Ch. 2 p.2-14/2-15: underflow triggers "If the stack pointer
// is less than 400[octal]" unless bit 0 of the stack pointer/limit is set
// to explicitly place the stack in page zero -- not done here). The fault
// handler it jumped to was location 3's *contents* (uninitialized, 0),
// producing PC=0 -- confirmed by single-stepping exactly up to the POPB
// and watching loc 40/61-65 hold all the right pre-pop values, then
// watching them go completely untouched and PC go to 0 the instant POPB
// executed. Fixed by raising `spval` to `0400` (matching the manual's own
// initialization rule: "start the stack at a location greater than
// 401[octal]"), comfortably clear of both this file's code and data.
//
// Assemble: dgasm -t eclipse_s140 -f simh -o trap_probe.simh trap_probe.s
// Run: dep PC 50, step 60, then inspect (matches the real verified
// transcript in TRAP_NOTES.md exactly):
//   e 221 (markerA)     -- 000111, argA as seen by path_a
//   e 222 (markerB)     -- 000222, argB as seen by path_b
//   e 223 (retA_seen)   -- 125252 (0xAAAA), call site 1's returned AC0
//   e 224 (retB_seen)   -- 052525 (0x5555), call site 2's returned AC0
//   e PC                -- 00077, the STA at the top of `handler` (SIMH's
//                           `step N` count lands there on this run; the
//                           program's own HALT has already executed --
//                           see TRAP_NOTES.md's full trace, which shows
//                           the HALT at address 076 with the "A" (real
//                           user-mode) trace prefix still present)

	org 050

_start:
	LDA 0, spval
	STA 0, 040		// stack pointer = 060 octal
	LDA 0, sl
	STA 0, 042		// stack limit = 177777 (overflow protection off)

	LDA 0, handleraddr
	STA 0, 2		// loc 2 = SC HANDLER ADDRESS (direct, bit0=0)

	LDA 0, doaval
	DOA 0, MAP		// MapStat=1: User Enable=1, map A

	LDA 0, zero
	LDA 1, one		// AC1 = 1 word to load
	ELEF 2, ptes		// AC2 = address OF ptes
	LMP			// Map[1][0] = identity (valid)

	LDA 0, @iptr		// TRIGGER: indirect reference flips Usermap=1
				// (page 0 identity-mapped, so this is
				// physically identical either way)

	// Genuinely in user mode from here.

	// --- call site 1: syscall reason 0 ---
	LDA 0, sysnumA
	LDA 1, argA
	SYC 1, 1		// voluntary trap -- NOT SYC 0,0 (see header)
call1_land:
	STA 0, retA_seen	// record what POPB actually put back in AC0

	// --- call site 2: syscall reason 1, a different call site ---
	LDA 0, sysnumB
	LDA 1, argB
	SYC 1, 1
call2_land:
	STA 0, retB_seen

	HALT			// both traps returned; normal flow resumed

handler:
	// Reached via PC = indirect(GetMap(2)). Usermap is already 0
	// (SYC forced it) -- ordinary logical=physical addressing here.
	STA 0, reason_tmp	// capture syscall number before AC0 is reused
	STA 1, arg_tmp		// capture the argument before AC1 is reused
	LDA 2, 040		// AC2 = current stack pointer (post-push);
				// AC0's pushed slot is at AC2-4 (see header)

	LDA 0, reason_tmp
	MOV# 0, 0, SZR		// skip next instruction if reason == 0
	JMP path_b		// reason != 0
	JMP path_a		// reason == 0

path_a:
	LDA 0, arg_tmp
	STA 0, markerA		// prove path_a ran, with the real argument
	LDA 0, retvalA
	STA 0, -4, 2		// fix up the STACK's AC0 slot -- POPB reads
				// this, not the live AC0 register
	JMP finish

path_b:
	LDA 0, arg_tmp
	STA 0, markerB
	LDA 0, retvalB
	STA 0, -4, 2

finish:
	LDA 0, doaval
	DOA 0, MAP		// re-set MapStat's User Enable bit -- SYC
				// cleared it; POPB only reactivates Usermap
				// if this bit is set again first
	POPB			// pop AC0-AC3 + PC, resume the user program
				// at the instruction after whichever SYC
				// trapped (verified, not assumed -- see
				// TRAP_NOTES.md)

	org 0200
	dev MAP = 03

zero:
	dw 0
one:
	dw 1
doaval:
	dw 1
spval:
	dw 0400
sl:
	dw 0177777
handleraddr:
	dw handler
ptes:
	dw 0			// logical page 0 -> physical page 0, identity
iptr:
	dw landing
landing:
	dw 999
sysnumA:
	dw 0
argA:
	dw 0111
sysnumB:
	dw 1
argB:
	dw 0222
reason_tmp:
	dw 0
arg_tmp:
	dw 0
retvalA:
	dw 0125252		// 0xAAAA -- distinguishes "path_a returned"
retvalB:
	dw 0052525		// 0x5555 -- distinguishes "path_b returned"
markerA:
	dw 0
markerB:
	dw 0
retA_seen:
	dw 0
retB_seen:
	dw 0
