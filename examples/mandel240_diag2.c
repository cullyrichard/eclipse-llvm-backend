#include <stdio.h>
#include <eclipse_io.h>
#include "fps.h"
#include "MANDEL240.h"

/* Continues from mandel240_diag.c (which confirmed load/start/halt all
 * work correctly on real hardware -- reached CHK9). This one adds the
 * DMA read-back and prints the RAW values it gets, so we can check
 * assumption #3 from mandel240_view.c's header comment directly:
 * does host_dma_in's transfer already hand back plain iteration counts
 * (small integers, 0..32), or does the raw AP-native 3-word MI<DB;
 * DB=SPFN encoding leak through instead (which would show up as
 * larger/oddly-patterned values, not small integers)?
 *
 * Reads back just the first 20 words of batch 0's data (columns 0-19
 * of row 0 -- MD address base+1 through base+20, skipping the
 * permanently-unwritten base+0 slot per MANDEL240_TRANSFER.md's own
 * "off-by-one gotcha"). Small enough to print in full and eyeball.
 */

#define MD_HALF_0 0000000

int main(void) {
    unsigned int buf[20];
    int i;

    printf("CHK0: entered main\n");

    IO_PULSE_CLEAR(077);
    IO_PULSE_PULSE(FPU_DEV);
    IO_PULSE_START(FPU_DEV);
    IO_PULSE_START(FPU_AP1);
    printf("CHK4: IO_PULSE done\n");

    load_psm(PROG_PS_ADDR, PROG_PS_SIZE, (const unsigned int *)PROG_ps);
    printf("CHK5: load_psm done\n");

    fpu_out(cmd_wtsr, PROG_PS_ADDR);
    fpu_out(cmd_wtfn, fn_load_tma);
    fpu_out(cmd_wtfn, fn_start);
    printf("CHK7: fn_start sent\n");

    while (!(fpu_in(cmd_rdfn) & fn_stop)) {
        /* spin */
    }
    printf("CHK9: AP halted (batch 0 complete)\n");

    host_dma_in((unsigned int)buf, 20, MD_HALF_0 + 1);
    printf("CHK10: DMA read-back complete\n");

    printf("Row 0, columns 0-19 (expect small integers, 0..32):\n");
    for (i = 0; i < 20; i++) {
        printf("  buf[%d] = %u (octal %o)\n", i, buf[i], buf[i]);
    }

    return 0;
}
