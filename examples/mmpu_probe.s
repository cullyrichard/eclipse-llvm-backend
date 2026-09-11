// mmpu_probe.s -- hand-written, hand-verified proof that the Eclipse
// S/140's MMPU (via SIMH's eclipse_cpu.c emulation) can redirect a
// single ESTA to a physical address beyond the 32768-word logical
// ceiling every compiled program is otherwise confined to. See
// MMPU_NOTES.md for the full writeup this test backs.
//
// Entirely supervisor-mode (Usermap stays 0 throughout) -- uses the
// "SingleCycle" single-instruction mapping mechanism (NIOP MAP arms
// it, the very next memory-referencing instruction consumes it and
// auto-reverts), not a full user-mode context switch. Assemble with:
//   dgasm -t eclipse_s140 -f simh -o mmpu_probe.simh mmpu_probe.s
// Run: dep PC 50, then step/run; marker is at logical/physical address
// 0104 octal (org 100 + 4 words: zero,one,markerval,pte). After HALT,
// `e 104` should show 0 (untouched -- Usermap=0 examines plain
// physical memory identically to marker's own logical address), and
// `e 120104` (physical page 50 octal << 10 | 0104's page-0 offset)
// should show 42 -- the mapped write actually landed there instead.

	org 050

_start:
	LDA 0, zero
	DOA 0, MAP		// MapStat=0: LMP targets User A map,
				// Enable=1 (User A) for the NIOP below

	LDA 1, one		// AC1 = 1 word to load
	ELEF 2, pte		// AC2 = address OF pte (not its value)
	LMP			// Map[1][0] = word at pte = physical page 50 octal

	LDA 0, markerval	// AC0 = 42, the value we're about to plant
				// at physical page 50 octal via the
				// single-cycle redirect below
	NIOP MAP		// arm SingleCycle = Enable (User A) for
				// exactly the next memory-referencing
				// instruction
	ESTA 0, marker, 0	// THE mapped write: logical address of
				// `marker` (page 0) is redirected through
				// Map[1][0] to physical page 50 octal for
				// this one instruction only, then
				// SingleCycle reverts
	HALT

	org 0100
	dev MAP = 03

zero:
	dw 0
one:
	dw 1
markerval:
	dw 42
pte:			// LMP page-table-entry word: logical slot 0 (bits
	dw 050		// 10-14) | physical page 50 octal = 40 decimal
			// (bits 0-9) -- physical words 40960-41983 decimal,
			// comfortably past the 32768-word logical ceiling
marker:			// where the mapped write lands *logically* (page 0,
	dw 0		// so untouched -- the real write goes to physical
			// page 50 octal instead, per the map entry above)
