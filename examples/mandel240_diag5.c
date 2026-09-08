#include <stdio.h>
#include <eclipse_io.h>
#include "fps.h"
#include "MANDEL240.h"

/* diag4.c hung on real hardware somewhere after CHK10 (DMA read-back),
 * with zero further output, for several minutes -- but the *identical*
 * computation logic (min/max tracking + distinct-value scan +
 * first-change scan), run on eclipseemu against the exact uniform-8.0
 * data diag3 found for the first 40 real pixels, completes instantly
 * and correctly (FAS/FSS/FCMP/FLAS/FFAS are real S/140 CPU
 * instructions, which eclipseemu DOES simulate, unlike the FPS-100/
 * device-054 board itself) -- see test_diag4_logic.c. So either the
 * real row isn't actually uniform past pixel 40 (some other bit
 * pattern trips something the emulator doesn't model faithfully), or
 * there's a genuine hardware-only FPU/bus quirk. This splits the work
 * into two clearly separated passes so a real-hardware hang can be
 * localized to one or the other, and prints the RAW (hi,lo) pair for
 * every single pixel unconditionally (not just decoded floats) so any
 * bit pattern that differs from the expected 16640/0 is visible even
 * if the second pass never gets there.
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
    int distinct_seen[64];
    int n_distinct = 0;
    int since_print;

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

    /* PASS 1: dump every pixel's raw (hi,lo) unconditionally -- no
     * float compare/convert at all, just printf/putchar. If this pass
     * doesn't finish, the hang is in the print path or the DMA'd data
     * itself, nothing to do with the hardware FPU. */
    printf("PASS1: raw dump start\n");
    since_print = 0;
    for (i = 0; i < IMG_WIDTH; i++) {
        printf("%d %u %u\n", i, buf[2 * i], buf[2 * i + 1]);
    }
    printf("PASS1: raw dump done\n");

    /* PASS 2: the actual diag4.c computation (min/max + distinct scan),
     * with a checkpoint every 50 pixels so a hang localizes to a
     * specific pixel index/value instead of "somewhere in 400". */
    printf("PASS2: compute start\n");
    minv = bits_to_float(buf[0], buf[1]);
    maxv = minv;
    since_print = 0;

    for (i = 0; i < IMG_WIDTH; i++) {
        float v = bits_to_float(buf[2 * i], buf[2 * i + 1]);
        int iv;
        int j, found;

        iv = (int)v;

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

        since_print++;
        if (since_print >= 50) {
            since_print = 0;
            printf("PASS2 checkpoint: i=%d iv=%d n_distinct=%d\n", i, iv, n_distinct);
        }
    }

    printf("PASS2: compute done, n_distinct=%d\n", n_distinct);
    printf("min = ");
    print_float(minv);
    printf("\nmax = ");
    print_float(maxv);
    putchar('\n');

    printf("ALL DONE\n");
    return 0;
}
