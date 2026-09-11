#ifndef _PMM_H
#define _PMM_H

/* Physical page frame allocator for the Eclipse S/140's real 1-megaword
 * physical memory -- the range reachable only through the MMPU
 * (examples/mmpu.h/mmpu.c), past the 32768-word logical ceiling every
 * ordinary compiled access is otherwise confined to. See PMM_NOTES.md
 * for the full page-size/frame-count derivation (re-checked directly
 * against SIMH's own eclipse_cpu.c/nova_defs.h source, not just quoted
 * from MMPU_NOTES.md's prior work) and real, hand-verified test output.
 *
 * - Frame size: 1024 words -- the same 1024-word granularity as the
 *   10-bit in-page offset every PTE already uses (mmpu.h's own
 *   physpage/offset split; MAP's `PAGEMASK` in eclipse_cpu.c is 10
 *   bits, 01777 octal = 1023 decimal).
 * - Frame count: 1024 total physical frames, numbered 0-1023.
 * - Frames 0-31 (physical words 0-32767) are permanently reserved and
 *   never handed out -- see PMM_NOTES.md's "Reserved range" section for
 *   why (the running program image, its stack, the interrupt vector
 *   page, and Map31's ambiguous default all live somewhere in that
 *   range).
 *
 * A "physical page number" returned by pmm_alloc_page() is exactly the
 * `physpage` argument mmpu_read_far()/mmpu_write_far() (mmpu.h) expect
 * -- no translation between the two APIs.
 */

#define PMM_FIRST_FRAME 32     /* first allocatable physical page number */
#define PMM_LAST_FRAME  1023   /* last allocatable physical page number */
#define PMM_NUM_FRAMES  (PMM_LAST_FRAME - PMM_FIRST_FRAME + 1)  /* 992 */

#define PMM_NONE (-1)   /* pmm_alloc_page()'s sentinel: no free frame left */

/* Must be called once before any alloc/free call -- marks every
 * allocatable frame free. */
void pmm_init(void);

/* Returns a free physical page number (PMM_FIRST_FRAME..PMM_LAST_FRAME)
 * and marks it used, or PMM_NONE if no frames remain free. Never
 * corrupts allocator state on exhaustion -- see PMM_NOTES.md's
 * exhaustion test. */
int pmm_alloc_page(void);

/* Marks physical page pfn free again. pfn outside
 * [PMM_FIRST_FRAME, PMM_LAST_FRAME] is silently ignored -- see pmm.c's
 * own comment on why this doesn't try to detect caller bugs. */
void pmm_free_page(int pfn);

/* Number of frames currently free. Diagnostic/test-only -- not needed
 * by a caller that just wants to alloc/free pages. */
int pmm_frames_free(void);

#endif
