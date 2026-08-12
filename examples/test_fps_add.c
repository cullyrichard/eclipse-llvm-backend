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

/* The FPU microprogram itself -- raw data, transcribed exactly from the
 * .asm's DATA SECTION, 14 four-word instructions (56 words total). Not
 * something to reinterpret as C logic: it's loaded into the FPU
 * verbatim, one word per fpu_out() call below. */
static const unsigned int fpu_program[] = {
    /* No-Op */
    0000000, 0000000, 0000000, 0000000,
    /* LDTMA;DB=!THIRD ; !THIRD is 04430 */
    0000003, 0103000, 0002000, 0004430,
    /* LDMA;DB=@077 */
    0000003, 0102000, 0002000, 0000077,
    /* LDDPA;DB=ZERO */
    0000003, 0104000, 0000000, 0000000,
    /* DPX(0)<DB;DB=TM */
    0000000, 0000000, 0047444, 0100000,
    /* LDTMA;DB=!SIXTN ; !SIXTN is 4451 */
    0000003, 0103000, 0002000, 0004451,
    /* No-Op */
    0000000, 0000000, 0000000, 0000000,
    /* DPY(0)<DB;DB=TM */
    0000000, 0000000, 0017444, 0100000,
    /* FADD DPX,DPY */
    0000001, 0123000, 0000444, 0100000,
    /* FADD */
    0000001, 0100000, 0000000, 0000000,
    /* MI<FA;INCMA */
    0000000, 0000000, 0000000, 0000120,
    /* No-Op */
    0000000, 0000000, 0000000, 0000000,
    /* No-Op */
    0000000, 0000000, 0000000, 0000000,
    /* Halt */
    0000003, 0170000, 0000000, 0000000,
};

#define FPU_PROGRAM_LEN (sizeof(fpu_program) / sizeof(fpu_program[0]))

int main(void) {
    unsigned int status;
    /* Loaded once, here, and referenced by value below -- not because
     * the loop needs it hoisted for any C-level reason, but as a
     * workaround for a real backend bug: in the unmodified version,
     * `main`'s waitstop loop referenced the raw `fn_stop` literal
     * directly inside the loop condition, and the compiler emitted the
     * *wrong* constant-pool entry there -- CPI0_2 (load_addr, 128)
     * instead of fn_stop's real value (0100000 octal = 32768). Verified
     * with an isolated single-constant test that materializing 32768
     * alone is correct, so this is specific to referencing a late
     * constant from inside a loop back-edge in a function with this
     * many distinct pool entries (~23 here), not a general boundary-
     * value bug. Loading it into a stable local once, outside the loop,
     * sidesteps whatever's going wrong at that specific use site. */
    unsigned int stop_bit = fn_stop;

    /* Defensively disable interrupts before anything else runs. Unlike
     * eclipseemu (which always starts from a clean Interrupt On=0 state
     * with nothing at memory location 1), real hardware carries state
     * across program loads: if Interrupt On was left enabled by
     * whatever ran here before -- e.g. this project's own
     * __attribute__((interrupt)) tests, which deliberately enable
     * interrupts and point memory location 1 at a handler -- a real
     * device interrupting mid-program would jump through that now-stale
     * vector into this program's unrelated memory layout. This program
     * has no interrupt handler of its own and never expects one, so
     * make sure none can fire. */
    IO_PULSE_CLEAR(077); /* NIOC 077 -- INTDS, disable interrupts */

    IO_PULSE_PULSE(FPU_DEV); /* NIOP 054 -- reset the FPU */
    IO_PULSE_START(FPU_DEV); /* NIOS 054 -- set the FPU to busy */

    /* Load the program's starting address into TMA. */
    fpu_out(cmd_wtsr, load_addr);
    fpu_out(cmd_wtfn, fn_load_tma);

    /* Load the microprogram onto the FPU, 4 words (one instruction) at
     * a time -- matches the .asm's loadpg loop exactly, cycling through
     * fn_load_ps_0..3 for each word of a 4-word group.
     *
     * This was fully unrolled with literal constant indices for a while,
     * as a workaround for a real backend bug: a runtime-variable index
     * (`fpu_program[i+k]`, `i` a loop variable) computed its address by
     * adding a byte-scaled offset directly to a word-granular pointer,
     * landing on element 2*i instead of element i. Root-caused and fixed
     * in the LLVM backend (EclipseTargetLowering::PerformDAGCombine) --
     * see DEBUGGING_NOTES.md -- and re-verified against eclipseemu with
     * this loop restored: 0/116 mismatches. */
    static const unsigned int fn_load_ps[4] = {fn_load_ps_0, fn_load_ps_1,
                                                fn_load_ps_2, fn_load_ps_3};
    unsigned int i;
    for (i = 0; i < FPU_PROGRAM_LEN; i += 4) {
        unsigned int k;
        for (k = 0; k < 4; k++) {
            fpu_out(cmd_wtsr, fpu_program[i + k]);
            fpu_out(cmd_wtfn, fn_load_ps[k]);
        }
    }

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

    /* Read back the results. The original halted after each register
     * read so an operator could inspect the accumulator on the
     * front-panel lights before continuing by hand; here we just print
     * each value instead of stopping. */
    fpu_out(cmd_wtsr, results_addr);
    fpu_out(cmd_wtfn, fn_load_ma);

    fpu_out(cmd_wtfn, fn_examine_regmd_o1);
    printf("regmd_o1: %o\n", fpu_in(cmd_rdlt));

    fpu_out(cmd_wtfn, fn_examine_regmd_o2);
    printf("regmd_o2: %o\n", fpu_in(cmd_rdlt));

    fpu_out(cmd_wtfn, fn_examine_regmd_o3);
    printf("regmd_o3: %o\n", fpu_in(cmd_rdlt));

    fpu_out(cmd_wtfn, fn_examine_regpsa);
    printf("regpsa: %o\n", fpu_in(cmd_rdlt));

    fpu_out(cmd_wtfn, fn_examine_regtma);
    printf("regtma: %o\n", fpu_in(cmd_rdlt));

    return 0;
}
