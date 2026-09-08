#include <stdio.h>
#include <eclipse_io.h>
#include "fps.h"
#include "MANDEL240.h"

/* Row 0 (an edge of the viewport) came back genuinely, entirely uniform
 * (8.0 across all 400 pixels, confirmed via mandel240_diag5.c under
 * --ieee) -- plausible for an edge row, but before trusting the full
 * driver rewrite, check a row near the vertical CENTER (row 120 of 240)
 * instead, where real Mandelbrot structure is far more likely if the
 * pipeline (load/run/DMA/decode) is genuinely working end to end.
 *
 * Row 120 = batch 3 (ROWS_PER_BATCH=40), row 0 within that batch. Batches
 * 0-2 are advanced through (halt + resume, per mandel240_view.c's own
 * "resume-after-halt" assumption) WITHOUT draining their DMA data --
 * we don't need it, and skipping it is harmless as long as resuming
 * really does just continue the AP's microsequencer in place.
 *
 * IMPORTANT: build this with --ieee -- see DEBUGGING_NOTES.md entry #34
 * (the default DG hardware-float mode produced silently wrong results,
 * likely a Nova 4 FPU vs. the assumed Eclipse S/140 one, on this real
 * machine).
 */

#define MD_HALF_0 0000000
#define MD_HALF_1 0040000
#define IMG_WIDTH 400
#define ROWS_PER_BATCH 40
#define TARGET_ROW 120  /* vertical center of a 240-row image */

static float bits_to_float(unsigned int hi, unsigned int lo) {
    union {
        unsigned long u;
        float f;
    } v;
    v.u = ((unsigned long)hi << 16) | (unsigned long)lo;
    return v.f;
}

static void wait_stop(void) {
    while (!(fpu_in(cmd_rdfn) & fn_stop)) {
        /* spin */
    }
}

static unsigned int buf[IMG_WIDTH * 2];

int main(void) {
    int target_batch = TARGET_ROW / ROWS_PER_BATCH;
    int row_in_batch = TARGET_ROW % ROWS_PER_BATCH;
    int batch;
    unsigned int half_base;
    unsigned int md_addr;
    int i;
    float minv, maxv;
    int distinct_seen[64];
    int n_distinct = 0;

    printf("CHK0: entered main, target_batch=%d row_in_batch=%d\n",
           target_batch, row_in_batch);

    IO_PULSE_CLEAR(077);
    IO_PULSE_PULSE(FPU_DEV);
    IO_PULSE_START(FPU_DEV);
    IO_PULSE_START(FPU_AP1);

    load_psm(PROG_PS_ADDR, PROG_PS_SIZE, (const unsigned int *)PROG_ps);

    for (batch = 0; batch <= target_batch; batch++) {
        if (batch == 0) {
            fpu_out(cmd_wtsr, PROG_PS_ADDR);
            fpu_out(cmd_wtfn, fn_load_tma);
            fpu_out(cmd_wtfn, fn_start);
        } else {
            fpu_out(cmd_wtfn, fn_start);
        }
        wait_stop();
        printf("CHK: batch %d halted\n", batch);
    }

    half_base = (target_batch & 1) ? MD_HALF_1 : MD_HALF_0;
    md_addr = half_base + 1 + (unsigned int)row_in_batch * (IMG_WIDTH * 2);

    host_dma_in((unsigned int)buf, IMG_WIDTH * 2, md_addr);
    printf("CHK: DMA read-back complete for row %d\n", TARGET_ROW);

    minv = bits_to_float(buf[0], buf[1]);
    maxv = minv;

    for (i = 0; i < IMG_WIDTH; i++) {
        float v = bits_to_float(buf[2 * i], buf[2 * i + 1]);
        int iv = (int)v;
        int j, found;

        if (v < minv) minv = v;
        if (v > maxv) maxv = v;

        found = 0;
        for (j = 0; j < n_distinct; j++) {
            if (distinct_seen[j] == iv) {
                found = 1;
                break;
            }
        }
        if (!found && n_distinct < 64) {
            distinct_seen[n_distinct] = iv;
            n_distinct++;
        }
    }

    printf("Row %d, all %d pixels:\n", TARGET_ROW, IMG_WIDTH);
    printf("  min = ");
    print_float(minv);
    printf("\n  max = ");
    print_float(maxv);
    printf("\n  distinct integer-ish values seen: %d\n", n_distinct);
    printf("  values: ");
    for (i = 0; i < n_distinct; i++) {
        printf("%d ", distinct_seen[i]);
    }
    putchar('\n');

    printf("  first 40 pixels: ");
    for (i = 0; i < 40; i++) {
        float v = bits_to_float(buf[2 * i], buf[2 * i + 1]);
        printf("%d ", (int)v);
    }
    putchar('\n');

    printf("ALL DONE\n");
    return 0;
}
