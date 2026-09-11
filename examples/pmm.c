#include "pmm.h"

/* One byte per allocatable frame -- deliberately NOT a packed 16-bit
 * bitmap. See PMM_NOTES.md's "Design" section for the full reasoning:
 * this backend has a *confirmed real* footgun around 16-bit shift
 * arithmetic that lands near/at the sign bit -- examples/mmpu_far_test.c's
 * own comment documents `physpage<<10 | offset` overflowing this
 * target's signed 16-bit `int` and triggering the compiler's own
 * -Wshift-overflow warning, at a magnitude a packed bitmap's own
 * per-word bit-index shift (`1 << 15`) would also reach. A foundational,
 * correctness-critical structure like this allocator is not where to
 * spend that risk to save under a kiloword of RAM -- plain
 * byte-per-frame array. freemap[i] != 0 means frame i is free.
 */
static unsigned char freemap[PMM_NUM_FRAMES];

void pmm_init(void) {
    int i;
    for (i = 0; i < PMM_NUM_FRAMES; i++) {
        freemap[i] = 1;
    }
}

int pmm_alloc_page(void) {
    int i;
    for (i = 0; i < PMM_NUM_FRAMES; i++) {
        if (freemap[i]) {
            freemap[i] = 0;
            return PMM_FIRST_FRAME + i;
        }
    }
    return PMM_NONE;
}

void pmm_free_page(int pfn) {
    int i;
    /* Out-of-range (and, silently, already-free/never-allocated) pfns
     * are ignored rather than asserted on -- there's no OS panic
     * mechanism for this allocator to call into yet (see PMM_NOTES.md).
     * A caller passing garbage is a caller bug this increment declines
     * to let corrupt allocator state, not a case it tries to detect. */
    if (pfn < PMM_FIRST_FRAME || pfn > PMM_LAST_FRAME) return;
    i = pfn - PMM_FIRST_FRAME;
    freemap[i] = 1;
}

int pmm_frames_free(void) {
    int i;
    int count = 0;
    for (i = 0; i < PMM_NUM_FRAMES; i++) {
        if (freemap[i]) count++;
    }
    return count;
}
