#include <eclipse_io.h>
#include <stdio.h>
#include "fps.h"

/* Converted from test_fps_add (3).asm. See fps.h for the device-054
 * driver primitives (fpu_out/fpu_in, the cmd_ and fn_ protocol
 * constants) -- this file only has what's specific to *this* test:
 * the microprogram data, the scratch addresses it stages things at,
 * and the read-back/print sequence. */

/* -- addresses staged into the FPU's switch register -- */
#define load_addr    0000200
#define results_addr 0000100

/* Placeholder settling delay between selecting a new FPU "examine"
 * target (fpu_out(cmd_wtfn, ...)) and reading it back (fpu_in(cmd_rdlt))
 * -- see the readback section in main() for why this exists. Not a
 * measured hardware timing figure; a plain busy-loop since this backend
 * has no usleep()/delay primitive. `volatile` keeps the compiler from
 * optimizing the whole loop away. */
#define SETTLE_ITERS 500
static void settle_delay(void) {
    volatile unsigned int i;
    for (i = 0; i < SETTLE_ITERS; i++) {
    }
}

/* The FPU microprogram itself -- raw data, transcribed exactly from the
 * .asm's DATA SECTION, 14 four-word instructions (56 words total). Not
 * something to reinter    fpu_out(cmd_wtsr, load_addr);
    fpu_out(cmd_wtfn, fn_load_tma);pret as C logic: it's loaded into the FPU
 * verbatim, one word per fpu_out() call below. */
 const unsigned int fpu_program[] = {
0000000,0000000,0000000,0000000,
0000003,00103000,0002000,0004451,
0000003,00102000,0002000,0000077,
0000003,00104000,0000000,0000000,
0000000,0000000,0057004,0100000,
0000000,0000000,0000440,0013000,
0000000,0000000,0000440,0013000,
0000000,0000000,0000440,0013000,
0000000,0000000,0000000,0000220,
0000000,0000000,0000000,0000000,
0000000,0000000,0000000,0000000,
0000003,00170000,0000000,0000000,
};

#define FPU_PROGRAM_LEN (sizeof(fpu_program) / sizeof(unsigned int))



int main(void) {
    unsigned int status;

    unsigned int stop_bit = fn_stop;

    IO_PULSE_CLEAR(077); /* NIOC 077 -- INTDS, disable interrupts */

    IO_PULSE_PULSE(FPU_DEV); /* NIOP 054 -- reset the FPU */
    IO_PULSE_START(FPU_DEV); /* NIOS 054 -- set the FPU to busy */

    load_psm(load_addr,FPU_PROGRAM_LEN,fpu_program);

     fpu_out(cmd_wtsr, load_addr);
    fpu_out(cmd_wtfn, fn_start);

    do {
        status = fpu_in(cmd_rdfn);
    } while ((status & stop_bit) == 0);
   /* Diagnostic: the loop above only checks the stop bit, discarding
     * every other bit of the status/function register -- if the FPU
     * stopped due to an error/exception condition rather than normal
     * completion, this is where that would show up. Not present in the
     * original .asm (which only had front-panel lights to inspect this
     * on, not a print). `status` is captured above already; the actual
     * printing is deferred and combined with the register read-back
     * prints below (see the single combined printf after all reads are
     * done) -- this doesn't change when `status` is read off the FPU,
     * only when its value gets printed. */

    /* Read back the results. The original halted after each register
     * read so an operator could inspect the accumulator on the
     * front-panel lights before continuing by hand -- which also gave
     * the FPU as much real time as the operator took to actually latch
     * the newly-selected word before it got read. Real-hardware testing
     * found regmd_o1/o2/o3 all reading back identical (while regpsa/
     * regtma, which aren't hit back-to-back with no gap, read back
     * correctly distinct) -- consistent with reading the MD word-select
     * before the FPU has settled on it. settle_delay() is a placeholder
     * busy-loop standing in for that lost pacing; not a real hardware
     * timing figure, just something to tune empirically -- increase
     * SETTLE_ITERS if regmd_o1/o2/o3 are still identical, or try
     * removing it entirely from just the o2/o3 calls if o1 alone is
     * already correct (would point at the load_ma -> first examine
     * transition specifically, not examine-to-examine in general). */
    fpu_out(cmd_wtsr, results_addr);
    fpu_out(cmd_wtfn, fn_load_ma);
    fps_word_struct return_word; 

    return_word = read_md(results_addr);

    fpu_out(cmd_wtfn, fn_examine_regpsa);
    //settle_delay();
    int psa = fpu_in(cmd_rdlt);

    fpu_out(cmd_wtfn, fn_examine_regtma);
    //settle_delay();
    int tma = fpu_in(cmd_rdlt);

    /* All register reads above are now captured in locals -- none of the
     * fpu_out/fpu_in device I/O or its ordering changed, only the
     * printing was deferred and combined here into a single printf (plus
     * the mandatory separate print_float() call, since this project's
     * printf() doesn't support %f) to cut down on page-zero format-
     * string literals. */
    printf("\nstatus: %o\nregmd_o1: %o\nregmd_o2: %o\nregmd_o3: %o\n"
           "regpsa: %o\nregtma: %o\ncalculated float value is: ",
           status, return_word.exp, return_word.mh, return_word.ml, psa, tma);
    print_float(calculate_value(return_word.exp,return_word.mh,return_word.ml));
    printf("\n");

    
return 0;
}
