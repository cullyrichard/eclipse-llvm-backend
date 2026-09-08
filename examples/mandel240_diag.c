#include <stdio.h>
#include <eclipse_io.h>
#include "fps.h"
#include "MANDEL240.h"

/* Diagnostic build: same startup sequence as mandel240_view.c, but with
 * a print checkpoint after every stage, up through the first HALT wait.
 * The goal is narrowing down exactly where "the machine halts right
 * away" is actually happening -- since _start is always `JSR
 * @main_SLOT,0` immediately followed by HALT, a fast halt most likely
 * means main() is returning (or crashing back to that point) almost
 * immediately, not that the CPU skips straight to a halt with nothing
 * having run. Whichever checkpoint is the LAST one you see printed
 * tells us which stage to look at next.
 *
 * Deliberately does NOT touch load_md/host_dma_in/the SIXEL logic at
 * all -- narrows the search to the four IO_PULSE_* calls, load_psm(),
 * and the very first fn_start + wait_stop(), which is the section most
 * likely to explain a near-instant halt given main() hasn't done
 * anything else yet at that point.
 */

int main(void) {
    printf("CHK0: entered main\n");

    IO_PULSE_CLEAR(077);
    printf("CHK1: after IO_PULSE_CLEAR(077)\n");

    IO_PULSE_PULSE(FPU_DEV);
    printf("CHK2: after IO_PULSE_PULSE(FPU_DEV)\n");

    IO_PULSE_START(FPU_DEV);
    printf("CHK3: after IO_PULSE_START(FPU_DEV)\n");

    IO_PULSE_START(FPU_AP1);
    printf("CHK4: after IO_PULSE_START(FPU_AP1)\n");

    load_psm(PROG_PS_ADDR, PROG_PS_SIZE, (const unsigned int *)PROG_ps);
    printf("CHK5: after load_psm (85 PS locations sent)\n");

    fpu_out(cmd_wtsr, PROG_PS_ADDR);
    fpu_out(cmd_wtfn, fn_load_tma);
    printf("CHK6: after loading TMA\n");

    fpu_out(cmd_wtfn, fn_start);
    printf("CHK7: after fn_start -- AP should be running now\n");

    printf("CHK8: about to poll for HALT (fn_stop) -- if you see this\n");
    printf("      but never CHK9, the AP itself never halts (or never\n");
    printf("      even started, if it was already halted/idle and this\n");
    printf("      poll just immediately sees a stale stop bit)\n");

    while (!(fpu_in(cmd_rdfn) & fn_stop)) {
        /* spin */
    }
    printf("CHK9: AP halted (batch 0 complete)\n");

    return 0;
}
