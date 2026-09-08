#include <stdio.h>
#include <eclipse_io.h>
#include "fps.h"
#include "MANDEL240.h"

/* Continues from mandel240_diag3.c: pixels 0-39 of row 0 were all
 * identical (8.0). Could be genuine (row 0 is likely one edge of the
 * viewport, and Mandelbrot edges often have wide uniform-escape
 * regions) or could mean the address math/decode is still off. This
 * reads the WHOLE row (400 pixels = 800 words) and reports min/max/
 * how many distinct values appear, rather than eyeballing individual
 * numbers -- if it's uniform across the ENTIRE row, that's suspicious;
 * if there's real variation somewhere, the decode is confirmed working
 * and row 0 just has a wide uniform stretch plus some real structure.
 */

#define MD_HALF_0 0000000
#define IMG_WIDTH 400

static float bits_to_float(unsigned int hi, unsigned int lo) {
    union {
        unsigned long u;
        float f;
    } v;
    v.u = ((unsigned long)hi << 16) | (unsigned long)lo;
    return v.f;
}

static unsigned int buf[IMG_WIDTH * 2];

int main(void) {
    int i;
    float minv, maxv;
    int distinct_seen[64]; /* escape counts 0..32 realistically, pad for safety */
    int n_distinct = 0;

    printf("CHK0: entered main\n");

    IO_PULSE_CLEAR(077);
    IO_PULSE_PULSE(FPU_DEV);
    IO_PULSE_START(FPU_DEV);
    IO_PULSE_START(FPU_AP1);

    load_psm(PROG_PS_ADDR, PROG_PS_SIZE, (const unsigned int *)PROG_ps);

    fpu_out(cmd_wtsr, PROG_PS_ADDR);
    fpu_out(cmd_wtfn, fn_load_tma);
    fpu_out(cmd_wtfn, fn_start);

    while (!(fpu_in(cmd_rdfn) & fn_stop)) {
        /* spin */
    }
    printf("CHK9: AP halted (batch 0 complete)\n");

    host_dma_in((unsigned int)buf, IMG_WIDTH * 2, MD_HALF_0 + 1);
    printf("CHK10: DMA read-back complete (%d words = %d pixels)\n",
           IMG_WIDTH * 2, IMG_WIDTH);

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

    printf("Row 0, all %d pixels:\n", IMG_WIDTH);
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

    /* Also show the first pixel where the value CHANGES from pixel 0's
     * value, if any -- pinpoints where any transition happens. */
    {
        float first = bits_to_float(buf[0], buf[1]);
        int change_at = -1;
        for (i = 1; i < IMG_WIDTH; i++) {
            float v = bits_to_float(buf[2 * i], buf[2 * i + 1]);
            if (v != first) {
                change_at = i;
                break;
            }
        }
        if (change_at >= 0) {
            printf("  first change from pixel 0's value at column %d: ", change_at);
            print_float(bits_to_float(buf[2 * change_at], buf[2 * change_at + 1]));
            putchar('\n');
        } else {
            printf("  no change anywhere across the whole row -- all %d pixels identical\n",
                   IMG_WIDTH);
        }
    }

    return 0;
}
