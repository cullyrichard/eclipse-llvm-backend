// boot_stage2.s -- second-stage 9-track tape bootstrap for the Eclipse LLVM
// backend. Assembled standalone (dgasm -f bin) and spliced into word offset
// 255 of tape record 1 by mktape.py -- see that file's header comment for
// the full boot sequence this is one half of.
//
// Why this exists / how record 1 ends up executing this at all:
// eclipseemu's (and real Eclipse hardware's) magtape boot ROM is generic
// across every "start pulse, wait, done" boot device -- SIMH's
// eclipse_cpu.c cpu_boot() loads the exact same ~32-word ROM for a paper
// tape reader boot and a magtape boot alike (confirmed by reading
// NOVA/eclipse_cpu.c and NOVA/nova_mta.c in the simh source). That ROM's
// own single NIOS start pulse to the magtape device (unlike a paper tape
// reader) doesn't hand back data frame-by-frame for the ROM's own
// byte-collection loop to poll -- SIMH's mta_svc() DMAs the *entire*
// first physical tape record directly into memory starting at address 0
// in one shot (nova_mta.c's CU_READ case: `M[pa] = (c1<<8)|c2`, pa
// starting at whatever mta_ma is, which is 0 on a fresh boot since
// nothing has ever DOB'd it). That silently overwrites the boot ROM's
// own code, including a self-modified trap at address 0377 (the ROM
// copies its own "JMP 377" instruction to memory location 0377 itself as
// a self-referential stall, then falls into it while the DMA completes
// in the background -- confirmed empirically with eclipseemu's `boot
// mta0`, single-stepped by hand: PC ends up spinning at address 0377
// executing "JMP 377" until the async record-read event fires and
// overwrites that exact word).
//
// So: whatever word ends up at address 0377 (255 decimal) once the DMA
// finishes is the *real* boot entry point -- there's no way to reach
// this file's own code except by living at that exact word offset within
// tape record 1. mktape.py handles the placement (record 1 = 256 words:
// a dummy word 0, then this program assembled for addresses 1-0377,
// truncated to fit). The `org 0377 / JMP stage2, 0` at the bottom of
// this file *is* that word.
//
// This program itself lives entirely in addresses 1-0047 -- Nova/Eclipse
// hardware never assigns user meaning to that range outside of an
// interrupt vector at 0/1 (irrelevant here: IORST, executed by the first
// boot ROM before any of this runs, leaves interrupts disabled, and this
// program never enables them) -- so it can't collide with anything a
// compiled program itself uses; every compiled program's own layout
// starts at 050 (see eclipse-cc/eclipse-compile.sh's own header
// comments -- a fixed, program-independent convention this file relies
// on directly, both as the DMA target for record 2 and as the address
// it jumps to when done).
//
// What it actually does: reset the magtape word-count register (a
// leftover nonzero count from record 1's own 256-word read would
// otherwise silently truncate record 2 -- see nova_mta.c's CU_READ: a
// too-small `wc` makes an oversized incoming record set STA_WCO and get
// cut down to the stale word count instead of its real length), point
// the magtape memory-address register at 050, issue one start pulse, and
// spin on "done" -- no per-word DIAS/status polling needed here (unlike
// the generic byte-serial boot ROM this file follows), since the
// controller's own DMA already deposited the words directly into memory
// by the time the done flag comes true. Once done, jump to 050 -- every
// program this toolchain produces starts there.
//
// Known limit: one NIOS pulse reads exactly one physical tape record in
// full (nova_mta.c's CU_READ, capped at MTA_MAXFR = 65536 bytes = 32768
// words); mktape.py puts the entire compiled program in that single
// second record, so a program whose image exceeds that is out of scope
// for this design (same spirit as the project's existing hard limits --
// e.g. the 256-word shared page-zero budget documented in
// SOFT_FLOAT_NOTES.md -- rather than something this file tries to work
// around with multi-record chaining).

	org 1
	dev MTA = 022

loadaddr:
	dw 050
zero:
	dw 0

stage2:
	LDA 0, zero			// AC0 = 0
	DOC 0, MTA			// mta_wc = 0 -> next read allows up to the
					// controller's own max record size, not
					// whatever record 1's 256-word read left
					// behind
	LDA 0, loadaddr			// AC0 = 050
	DOB 0, MTA			// mta_ma = 050 -- record 2 lands exactly
					// where every compiled program expects
					// to start
	NIOS MTA			// start pulse: read record 2 (async)
poll:
	SKPDN MTA
	JMP poll
	JMP 050, 0			// every compiled program's entry point

	org 0377
	JMP stage2, 0			// word offset 255 of tape record 1:
					// this is where the boot ROM's own
					// trap ends up once the DMA finishes
					// -- see this file's header comment

