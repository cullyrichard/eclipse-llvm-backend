#include <stdio.h>
#include "pmm.h"
#include "mmpu.h"

/* Real, hand-predicted-then-confirmed exercise of examples/pmm.h's
 * physical frame allocator through the full eclipse-cc pipeline, using
 * examples/mmpu.h's already-verified far-pointer API as the actual
 * proof mechanism: every allocated frame gets a distinct pattern
 * written via mmpu_write_far() and read back via mmpu_read_far(), so a
 * passing run proves the allocator's returned pfns are real,
 * independently addressable physical memory (each one reached past the
 * 32768-word logical ceiling, exactly like mmpu_far_multi_test.c), not
 * just bitmap bookkeeping that happens to look right.
 *
 * Every printed value below was worked out by hand from pmm.c's
 * first-fit (lowest free index) allocation policy *before* running
 * this program -- see PMM_NOTES.md for the worked predictions and the
 * real transcript they were checked against.
 */
int main(void) {
    int i;
    int pfns[6];
    int newpfn_a, newpfn_b;
    int drained, got, over, recovered;

    pmm_init();
    printf("free_at_start=%d\n", pmm_frames_free());

    /* Allocate 6 frames; write a distinct pattern (1000+i) to each via
     * the real MMPU far-pointer API. */
    for (i = 0; i < 6; i++) {
        pfns[i] = pmm_alloc_page();
        mmpu_write_far(pfns[i], 0, 1000 + i);
    }
    printf("pfn0=%d pfn1=%d pfn2=%d pfn3=%d pfn4=%d pfn5=%d\n",
           pfns[0], pfns[1], pfns[2], pfns[3], pfns[4], pfns[5]);
    printf("free_after_6allocs=%d\n", pmm_frames_free());

    for (i = 0; i < 6; i++) {
        printf("before_free[%d]=%d\n", i, mmpu_read_far(pfns[i], 0));
    }

    /* Free two of the six (indices 1 and 3), reallocate two more, and
     * confirm: the freed frames come back out first (first-fit, lowest
     * free index), writing the reallocated frames' new pattern doesn't
     * disturb the frames still in use, and the still-in-use frames'
     * original data survives untouched. */
    pmm_free_page(pfns[1]);
    pmm_free_page(pfns[3]);
    printf("free_after_2frees=%d\n", pmm_frames_free());

    newpfn_a = pmm_alloc_page();
    newpfn_b = pmm_alloc_page();
    printf("realloc_a=%d realloc_b=%d\n", newpfn_a, newpfn_b);
    printf("free_after_2reallocs=%d\n", pmm_frames_free());

    mmpu_write_far(newpfn_a, 0, 5001);
    mmpu_write_far(newpfn_b, 0, 5002);

    printf("readback_a=%d readback_b=%d\n",
           mmpu_read_far(newpfn_a, 0), mmpu_read_far(newpfn_b, 0));

    printf("still0=%d still2=%d still4=%d still5=%d\n",
           mmpu_read_far(pfns[0], 0), mmpu_read_far(pfns[2], 0),
           mmpu_read_far(pfns[4], 0), mmpu_read_far(pfns[5], 0));

    /* Exhaustion test: drain every remaining frame through the
     * allocator's own bookkeeping (no MMPU calls needed here -- this is
     * testing pmm.c's bitmap, not the MMPU driver again), confirm the
     * drained count matches PMM_NUM_FRAMES minus what's already
     * allocated, confirm over-allocating past exhaustion returns
     * PMM_NONE cleanly rather than corrupting state (free count
     * unchanged by the failed call), then confirm freeing one frame
     * makes the allocator usable again. */
    drained = 0;
    while ((got = pmm_alloc_page()) != PMM_NONE) {
        drained++;
    }
    printf("drained=%d free_after_drain=%d\n", drained, pmm_frames_free());

    over = pmm_alloc_page();
    printf("over_alloc=%d free_after_overalloc_attempt=%d\n",
           over, pmm_frames_free());

    pmm_free_page(newpfn_a);
    printf("free_after_1free=%d\n", pmm_frames_free());
    recovered = pmm_alloc_page();
    printf("recovered=%d free_final=%d\n", recovered, pmm_frames_free());

    return 0;
}
