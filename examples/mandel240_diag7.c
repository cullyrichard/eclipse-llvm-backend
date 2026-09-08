#include <stdio.h>
#include <eclipse_io.h>
#include "fps.h"
#include "MANDEL240.h"

/* mandel240_diag6.c's row 120 (batch 3, via 3 resumes) came back looking
 * like garbage -- 399/400 pixels decoding to a flat 0.0 and one wild
 * outlier near -2^31 in magnitude -- not the smooth small-integer
 * variation real Mandelbrot escape counts should show. That's much more
 * consistent with reading UNINITIALIZED/wrong memory than with real
 * data, which points straight at mandel240_view.c's own flagged
 * assumption #1: does a bare fn_start (no TMA reload) actually resume
 * the AP's microsequencer where it left off, or does something else
 * happen (restart, no-op, garbage)?
 *
 * Direct test, in two independent parts:
 *
 * 1. Read the AP's own PSA (program status address / PC) register via
 *    fn_examine_regpsa + cmd_rdlt (already used elsewhere in this
 *    project -- fib.c, test_fps_add.c, fps_dma_test.c -- just never in
 *    the MANDEL240 driver itself) right after batch 0 halts, and again
 *    after each subsequent resume+halt. If resume is working, later
 *    PSA values should differ from the batch-0 one in a way consistent
 *    with genuine forward progress through the microprogram.
 *
 * 2. Read row 0 of MD_HALF_0 BEFORE any resume (confirmed uniform 8.0
 *    by mandel240_diag3.c/diag5.c) and AGAIN after batch 2 (which reuses
 *    MD_HALF_0, since batches ping-pong even/odd between the two
 *    halves) -- if resume is broken and batch 2 just repeats whatever
 *    batch 0 did, this second read will come back byte-for-byte
 *    identical to the first. If resume genuinely advances computation,
 *    it should differ (whether or not it's the CORRECT next values is
 *    a separate question this specific test can't answer, but "does it
 *    even change" is the more basic, decisive question first).
 *
 * Build with --ieee -- see DEBUGGING_NOTES.md entry #34.
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

static void wait_stop(void) {
    while (!(fpu_in(cmd_rdfn) & fn_stop)) {
        /* spin */
    }
}

static int read_psa(void) {
    fpu_out(cmd_wtfn, fn_examine_regpsa);
    return fpu_in(cmd_rdlt);
}

static unsigned int buf[IMG_WIDTH * 2];

static void report_row(const char *label) {
    int i;
    float minv, maxv;
    int n_same_as_first = 0;
    float first = bits_to_float(buf[0], buf[1]);

    minv = first;
    maxv = first;
    for (i = 0; i < IMG_WIDTH; i++) {
        float v = bits_to_float(buf[2 * i], buf[2 * i + 1]);
        if (v < minv) minv = v;
        if (v > maxv) maxv = v;
        if (v == first) n_same_as_first++;
    }

    printf("%s: pixel0 hi=%u lo=%u, min=", label, buf[0], buf[1]);
    print_float(minv);
    printf(" max=");
    print_float(maxv);
    printf(" same_as_pixel0=%d/%d\n", n_same_as_first, IMG_WIDTH);
}

int main(void) {
    int psa0, psa1, psa2;

    printf("CHK0: entered main\n");

    IO_PULSE_CLEAR(077);
    IO_PULSE_PULSE(FPU_DEV);
    IO_PULSE_START(FPU_DEV);
    IO_PULSE_START(FPU_AP1);

    load_psm(PROG_PS_ADDR, PROG_PS_SIZE, (const unsigned int *)PROG_ps);

    fpu_out(cmd_wtsr, PROG_PS_ADDR);
    fpu_out(cmd_wtfn, fn_load_tma);
    fpu_out(cmd_wtfn, fn_start);
    wait_stop();
    psa0 = read_psa();
    printf("CHK: batch 0 halted, psa0=%o\n", psa0);

    host_dma_in((unsigned int)buf, IMG_WIDTH * 2, MD_HALF_0 + 1);
    report_row("BEFORE (batch 0, half0)");

    fpu_out(cmd_wtfn, fn_start);
    wait_stop();
    psa1 = read_psa();
    printf("CHK: batch 1 halted, psa1=%o\n", psa1);

    fpu_out(cmd_wtfn, fn_start);
    wait_stop();
    psa2 = read_psa();
    printf("CHK: batch 2 halted, psa2=%o\n", psa2);

    host_dma_in((unsigned int)buf, IMG_WIDTH * 2, MD_HALF_0 + 1);
    report_row("AFTER (batch 2, half0)");

    printf("psa0=%o psa1=%o psa2=%o (all equal -> resume never advances PSA)\n",
           psa0, psa1, psa2);

    printf("ALL DONE\n");
    return 0;
}
