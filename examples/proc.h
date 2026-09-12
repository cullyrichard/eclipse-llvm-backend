#ifndef _PROC_H
#define _PROC_H

/* Process Control Block layout for the S/140 two-process (map A / map B)
 * preemptive round-robin scheduler -- see PROCESS_NOTES.md for the full
 * design writeup and real, single-stepped/traced verification evidence,
 * and examples/scheduler_probe.s for the actual (hand-assembled, not
 * C-compiled) implementation this header documents.
 *
 * This header is descriptive, not compiled: the scheduler itself is
 * hand-written Eclipse S/140 assembly (examples/scheduler_probe.s), the
 * same reason examples/mmpu_probe.s/trap_probe.s/mmpu_*_probe.s are all
 * .s files rather than C -- the interrupt entry point, the MapIntMode
 * DIA-must-be-first contract, and the carry-flag save/restore this
 * scheduler needs have no C-level expression on this backend (there is
 * no C-callable equivalent of "read the CPU carry flag" or "issue a
 * bare indirect JMP as an MMPU mode-switch trigger"). This header exists
 * so the TCB's field layout is written down in one place, in the same
 * spirit as examples/mmpu.h/pmm.h document their own C-callable
 * counterparts' contracts, even though nothing here is `#include`d by
 * anything.
 *
 * ---- Why exactly 2 processes, not N ----
 *
 * The real S/140 MMPU has exactly two physical user address map slots,
 * A and B (MMPU_NOTES.md's Phase 1 section, confirmed against the manual's
 * own page images: "The MMPU can hold two user maps, but only one can be
 * enabled at any one time," and DOA's own 3-bit Map Select field marking
 * every code beyond A/B as hardware-"Reserved"). A TCB in this design is
 * not a generic "virtual address space" the way a modern OS's task_struct
 * would be -- it is a saved register/PC snapshot bound PERMANENTLY to one
 * of exactly two hardware map slots. There is no field here for "which
 * page table this process uses" because there is no way to give a third
 * process a page table of its own without evicting one of A or B's
 * *contents* -- a real, separate, harder problem (map-content swapping),
 * deliberately out of scope for this increment. See PROCESS_NOTES.md's
 * "Future work" section.
 *
 * ---- Field layout ----
 *
 * Each TCB is 6 words, saved/restored by scheduler_probe.s's interrupt
 * handler using plain (non-indexed) STA/LDA to two hardcoded, statically
 * -allocated 6-word blocks (tcbA_*/tcbB_*) -- not a generic pointer-
 * indexed structure -- because there are only ever exactly two of them,
 * matching this project's established style of hand-unrolling a 2-way
 * choice (see mmpu_intmode_resume_probe.s's naive/informed resume paths,
 * trap_probe.s's path_a/path_b dispatch) rather than building indirection
 * machinery for a fixed cardinality of 2.
 *
 *   offset  name       meaning
 *   ------  --------   -------------------------------------------------
 *   +0      ac0        AC0 at the moment this process was last preempted
 *   +1      ac1        AC1 (this design's "running sum" register)
 *   +2      ac2        AC2 (this design's "step constant" register)
 *   +3      ac3        AC3 (this design's "iteration count" register)
 *   +4      pc         Saved resume PC, in the SAME format the hardware's
 *                       own SYC/fault return-block push already uses
 *                       (TRAP_NOTES.md, MMPU_NOTES.md's page-fault
 *                       section): the low 15 bits are the logical
 *                       address to resume at; bit 15 (0100000 octal) is
 *                       the saved carry flag, folded in by hand since
 *                       -- unlike SYC/fault, which the HARDWARE pushes
 *                       automatically -- a plain device interrupt (the
 *                       vector this scheduler uses) does no automatic
 *                       register push at all; the handler has to do it,
 *                       the same reason examples/isr_c_test.c's compiled
 *                       __attribute__((interrupt)) handler has its own
 *                       AC0/AC1 callee-save prologue/epilogue.
 *   +5      mapstat    The exact DOA-loadable MapStat word to reprogram
 *                       when resuming this process: bit 0 (User Enable)
 *                       + bit 13 (A/B select). For process A this is
 *                       always 1 (enterA); for process B, always 5
 *                       (enterB) -- see MMPU_NOTES.md's "two-user-map
 *                       context switch" section for the bit derivation.
 *                       Not re-derived at save time from first
 *                       principles -- it is a straight copy of whatever
 *                       DIA's MapIntMode-informed readback returned at
 *                       interrupt entry (see PROCESS_NOTES.md's
 *                       discussion of why this equals the original
 *                       DOA-loaded constant bit-for-bit, per
 *                       mmpu_intmode_resume_probe.s's already-verified
 *                       finding), reused here rather than re-verified,
 *                       since that finding is this whole scheduler's
 *                       load-bearing precondition.
 *
 * What is deliberately NOT in this TCB: a saved AC2-as-frame-pointer
 * convention (this is hand-written assembly with no compiled call
 * frames to preserve, unlike examples/mmpu.c's C-callable API), any
 * page-table/PTE data (both maps are loaded once, at boot, and never
 * reloaded -- see PROCESS_NOTES.md's "process lifecycle" scope note),
 * and any priority/nice-value/scheduling-class field (the scheduler is
 * unconditional strict alternation between exactly 2 processes -- see
 * PROCESS_NOTES.md's "Scheduling policy" section for why that is a
 * complete policy for N=2, not a simplification of a larger one).
 */

#define TCB_AC0      0
#define TCB_AC1      1
#define TCB_AC2      2
#define TCB_AC3      3
#define TCB_PC       4
#define TCB_MAPSTAT  5
#define TCB_SIZE     6

/* The two hardware-defined MapStat/DOA values a TCB's mapstat field can
 * ever legally hold on the real S/140 -- see MMPU_NOTES.md's "two-user-
 * map context switch" section for the bit derivation (bit 0 = User
 * Enable, bit 13 = A/B select, 04 octal = bit 13 alone). */
#define TCB_MAPSTAT_A  1   /* User Enable=1, A/B=0 (User A) */
#define TCB_MAPSTAT_B  5   /* User Enable=1, A/B=1 (User B) */

#endif
