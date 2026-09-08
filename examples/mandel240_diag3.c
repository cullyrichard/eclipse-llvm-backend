#include <stdio.h>
#include <eclipse_io.h>
#include "fps.h"
#include "MANDEL240.h"

/* Continues from mandel240_diag2.c, which showed row 0's first 20
 * words as a perfectly repeating (16640, 0) pair -- combining those
 * two 16-bit words into one 32-bit value (0x40800000) and reading it
 * as this target's normal `float` bit layout gives EXACTLY 4.0, no
 * fractional garbage. That suggests each pixel's escape count is
 * stored as a 2-WORD FLOAT, not a 1-word plain integer -- i.e. MD
 * holds 2 words per pixel, not 1, which also means the row/column
 * addressing in mandel240_view.c is currently off by a factor of 2.
 *
 * This reads further into row 0 (80 raw words = 40 pixels at 2 words
 * each) and decodes each pair as a float via this target's existing
 * sf_from_bits-equivalent reinterpretation, printing both the raw hex
 * words and the decoded value side by side -- if escape counts really
 * are stored this way, the decoded values should look like small,
 * whole-number floats (0.0 to 32.0) that plausibly change from pixel
 * to pixel (not all identical the way the first 20 raw *words*
 * looked, which was really just 10 identical *pixels* in a row).
 */

#define MD_HALF_0 0000000

static float bits_to_float(unsigned int hi, unsigned int lo) {
    union {
        unsigned long u;
        float f;
    } v;
    v.u = ((unsigned long)hi << 16) | (unsigned long)lo;
    return v.f;
}

int main(void) {
    unsigned int buf[80];
    int i;

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

    host_dma_in((unsigned int)buf, 80, MD_HALF_0 + 1);
    printf("CHK10: DMA read-back complete (80 words = 40 pixels @ 2 words each)\n");

    printf("Row 0, pixels 0-39, as (hi,lo) word pairs and decoded float:\n");
    for (i = 0; i < 40; i++) {
        unsigned int hi = buf[2 * i];
        unsigned int lo = buf[2 * i + 1];
        float v = bits_to_float(hi, lo);
        printf("  pixel %d: hi=%o lo=%o  -> ", i, hi, lo);
        print_float(v);
        putchar('\n');
    }

    return 0;
}
