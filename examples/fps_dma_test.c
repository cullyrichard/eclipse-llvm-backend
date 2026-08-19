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
#define results_addr 0000000

/* The FPS program itself -- raw data, transcribed exactly from the
 * .asm's DATA SECTION, 14 four-word instructions (56 words total). Not
 * something to reinterpret as C logic: it's loaded into the FPU
 * verbatim, one word per fpu_out() call below. */
static const unsigned int fpu_program[] = {
//;No-Op
0000000,0000000,0000000,0000000,
//;LDTMA;DB=!THIRD ; !THIRD is 04430
0000003,0103000,0002000,0004430,
//;LDMA;DB=@077
0000003,0102000,0002000,0000077,
//;LDDPA;DB=ZERO
0000003,0104000,0000000,0000000,
//;DPX(0)<DB;DB=TM
0000000,0000000,0047444,0100000,
//;LDTMA;DB=!SIXTN ; !SIXTN is 4451
0000003,0103000,0002000,0004451,
//;No-Op
0000000,0000000,0000000,0000000,
//;DPY(0)<DB;DB=TM
0000000,0000000,0017444,0100000,
//;FADD DPX,DPY
0000001,0123000,0000444,0100000,
//;FADD
0000001,0100000,0000000,0000000,
//;MI<FA;INCMA
0000000,0000000,0000000,0000120,
//;No-Op
0000000,0000000,0000000,0000000,
//;No-Op
0000000,0000000,0000000,0000000,
//;Halt
0000003,0170000,0000000,0000000,
};

//const unsigned int *fpu_pgm_ptr = fpu_program;
static const unsigned int fpu_md[] = {
// 3.0
0001002,0003000,000000,
// 4.0
0001003,0002000,0000000,
// 5.0
0001003,0002400,0000000,
// 6.0
0001003,0003000,0000000,
// 7.0
0001003,0003400,0000000,

};
const unsigned int *fpu_md_ptr = &fpu_md[0];
#define FPU_PRGM_LEN (sizeof(fpu_program) / sizeof(fpu_program[0]))
#define FPU_MD_LEN (sizeof(fpu_md)/sizeof(fpu_md[0]))

//define a returned FPS word within a struct so that we can individually manipulate each value
const unsigned int hma_high = 0;
int main(void) {
    unsigned int status;

    unsigned int stop_bit = fn_stop;
    fps_word_struct fps_output_word;
    IO_PULSE_CLEAR(077); /* NIOC 077 -- INTDS, disable interrupts */

    IO_PULSE_PULSE(FPU_DEV); /* NIOP 054 -- reset the FPU */
    IO_PULSE_START(FPU_DEV); /* NIOS 054 -- set the FPU to busy */

    /* Load the program's starting address into TMA. */
    load_psm(load_addr,FPU_PRGM_LEN,fpu_program); 
    load_md(0,FPU_MD_LEN,fpu_md);
    //fps_dma_host_out(fpu_md,0,sizeof(fpu_md)); 

    /* Start the FPU running, at the address staged in the switch
     * register (load_addr, same as the TMA load above). */
    fpu_out(cmd_wtsr, load_addr);
    fpu_out(cmd_wtfn, fn_start);
    /* Wait until the FPU reports stopped (fn_stop bit set in the
     * function/status register) -- matches the .asm's AND#/SNR
     * skip-test-and-loop idiom. */
    do {
        status = fpu_in(cmd_rdfn);
    } while ((status & stop_bit) == 0);
    for(int md_test_iterator = 0; md_test_iterator < 5; md_test_iterator++){ 
            fps_output_word = read_md(md_test_iterator); 
            print_float(calculate_value(fps_output_word.exp,fps_output_word.mh,fps_output_word.ml));
    }


    





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
           status, fps_output_word.exp, fps_output_word.mh, fps_output_word.ml, psa, tma);
    print_float(calculate_value(fps_output_word.exp,fps_output_word.mh,fps_output_word.ml));
    printf("\n");

    
return 0;
}
