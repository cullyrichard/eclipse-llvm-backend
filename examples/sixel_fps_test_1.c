#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <eclipse_io.h>
#include "fps.h"

#define results_addr 1  /* MD results start at MD address 1, not 0 */

#define IMG_WIDTH     200
#define IMG_HEIGHT    120
#define BAND_ROWS     6
#define BAND_WORDS    (IMG_WIDTH * BAND_ROWS)   /* 1200 */
#define NUM_BANDS     (IMG_HEIGHT / BAND_ROWS)  /* 20 */
#define PALETTE_COUNT 16
#define load_addr  200
#define FPU_PRGM_LEN 204

static const unsigned int fpu_prgm[204] = {
    0000000, 0000000, 0000000, 0000000,
    0000003, 0104000, 0002000, 0000004,
    0011314, 0000000, 0010001, 0000056,
    0011314, 0000000, 0040001, 0000056,
    0011314, 0000000, 0010000, 0000052,
    0011314, 0000000, 0010005, 0000052,
    0001620, 0000000, 0002000, 0000200,
    0000000, 0000000, 0000000, 0000000,
    0000003, 0104000, 0002000, 0000004,
    0011314, 0000000, 0040006, 0000051,
    0000000, 0000000, 0044005, 0000000,
    0001610, 0000000, 0002000, 0000310,
    0011010, 0000000, 0000000, 0000034,
    0000000, 0000000, 0052004, 0000000,
    0040400, 0000000, 0000000, 0000000,
    0001604, 0000000, 0002000, 0000000,
    0000000, 0000000, 0000400, 0012400,
    0000000, 0000000, 0000040, 0015000,
    0000000, 0000000, 0000440, 0013000,
    0000000, 0000000, 0140007, 0017400,
    0000000, 0112000, 0030700, 0177400,
    0000001, 0123000, 0140773, 0000000,
    0000001, 0122000, 0020300, 0060000,
    0000001, 0132000, 0020530, 0040000,
    0000001, 0032000, 0100622, 0000000,
    0000001, 0123000, 0100254, 0000000,
    0000001, 0100000, 0000000, 0000000,
    0001200, 0000562, 0020000, 0100000,
    0001104, 0000744, 0000000, 0000000,
    0040104, 0000000, 0006000, 0000320,
    0001210, 0000000, 0000000, 0000000,
    0000000, 0000765, 0000000, 0000000,
    0001611, 0132000, 0002150, 0000310,
    0000001, 0100000, 0044005, 0000000,
    0000000, 0000000, 0020000, 0120000,
    0000000, 0000340, 0000000, 0000000,
    0000001, 0123000, 0000510, 0000000,
    0000001, 0100000, 0000000, 0000000,
    0000000, 0000000, 0100005, 0000000,
    0000000, 0000340, 0000000, 0000000,
    0000003, 0102000, 0002000, 0000000,
    0001614, 0000000, 0002000, 0056700,
    0011014, 0000000, 0000000, 0177743,
    0001214, 0000000, 0000000, 0000000,
    0000000, 0000756, 0000000, 0000000,
    0000003, 0170000, 0000000, 0000000,
    0000000, 0000040, 0025400, 0000000,
    0000000, 0000040, 0015463, 0031463,
    0000000, 0000037, 0132436, 0134122,
    0000000, 0000037, 0132436, 0134122,
    0000000, 0000040, 0032000, 0000000,
};

/* One shared band buffer, reused for every band: 1200 words (~3.7% of
 * the 32K-word address space) instead of 24000 words (~73%) for the
 * whole image resident at once -- the reason no MMU/extended
 * addressing is needed here. */
static unsigned int band_buf[BAND_WORDS];

/* Row-index -> sixel bit in place of `1u << row`: this backend only
 * supports shifts by a compile-time constant amount. */
static uint8_t sixel_bit(int row) {
    if (row == 0) return 1;
    if (row == 1) return 2;
    if (row == 2) return 4;
    if (row == 3) return 8;
    if (row == 4) return 16;
    return 32;
}

/* Fixed 16-color palette (same as this project's earlier tests) --
 * ESC kept out of the string literal on purpose, see send_sixel_from_fps's
 * own note below. */
#define SIXEL_PALETTE_BODY \
    "P7q" \
    "#0;2;0;0;0#1;2;10;3;10#2;2;4;0;18#3;2;2;2;29" \
    "#4;2;0;3;39#5;2;5;17;54#6;2;9;32;69#7;2;22;49;82" \
    "#8;2;53;71;90#9;2;83;93;97#10;2;95;91;75#11;2;97;79;37" \
    "#12;2;100;67;0#13;2;80;50;0#14;2;60;34;0#15;2;26;12;6"

/* Streams a 200x120 sixel image straight to the console, pulling
 * pixel data from the FPS via DMA one 6-row band at a time instead of
 * buffering the whole 24000-word image in Eclipse memory at once.
 * Each band reuses the same 1200-word band_buf.
 *
 * `fps_results_base` is the FPS-side source address for band 0 (same
 * role as dma13.c's own `results_addr` constant) -- each of the 20
 * bands reads from fps_results_base + band*BAND_WORDS, so this
 * assumes the FPS holds/stages the full 24000-word image on its own
 * side and hands it out in consecutive BAND_WORDS-sized chunks as
 * `results_addr` advances. That offset convention matches how
 * host_dma_in/results_addr is used elsewhere in this project
 * (dma13.c), but wasn't independently re-verified against real FPS
 * hardware here -- confirm it matches your FPS-side program before
 * relying on it for real capture.
 *
 * ESC is sent via a standalone putchar(27), never inside a string
 * literal -- this backend emits non-printable bytes inside string
 * *constants* as an invalid hex escape that dgasm's assembler either
 * rejects outright or (worse) silently corrupts; see this project's
 * earlier findings on send_sixel.c/sixel.c for the confirmed
 * mechanism. A bare putchar() argument is a plain immediate load, not
 * a string constant, so it isn't affected.
 *
 * band_buf and present[] are plain arrays, not arrays of struct:
 * `struct_array[i].field` with a runtime-varying `i` was confirmed
 * broken on this backend (see send_sixel.c/sixel.c again); plain
 * array indexing with a runtime index does not have this problem
 * (also confirmed -- dma13.c's own return_data[j]/return_data[j+1]
 * loop uses exactly this pattern successfully).
 */
void send_sixel_from_fps(unsigned int fps_results_base) {
    printf("\n\n\n\n");
    
    putchar(27);
    printf("%s", SIXEL_PALETTE_BODY);

    uint8_t present[PALETTE_COUNT];

    for (int band = 0; band < NUM_BANDS; band++) {
        /* ctl_fmt_1: single-word-per-value format, matching how the
         * FPS delivers these indices (one per 16-bit word, not
         * paired like dma13.c's own 2-word float format, which uses
         * ctl_fmt_3). */
        host_dma_in((unsigned int)band_buf, BAND_WORDS,
                    fps_results_base + band * BAND_WORDS, ctl_fmt_1);

        for (int c = 0; c < PALETTE_COUNT; c++) present[c] = 0;
        for (int i = 0; i < BAND_WORDS; i++) {
            unsigned int v = band_buf[i];
            if (v < PALETTE_COUNT) present[v] = 1;
        }

        int is_first_color = 1;
        for (int c = 0; c < PALETTE_COUNT; c++) {
            if (!present[c]) continue;
            if (!is_first_color) putchar('$');
            is_first_color = 0;
            printf("#%d", c);

            int x = 0;
            while (x < IMG_WIDTH) {
                uint8_t mask = 0;
                for (int r = 0; r < BAND_ROWS; r++) {
                    if (band_buf[r * IMG_WIDTH + x] == (unsigned int)c) {
                        mask |= sixel_bit(r);
                    }
                }
                char ch = (char)(mask + 0x3F);

                int run = 1;
                while (x + run < IMG_WIDTH) {
                    uint8_t mask2 = 0;
                    for (int r = 0; r < BAND_ROWS; r++) {
                        if (band_buf[r * IMG_WIDTH + (x + run)] == (unsigned int)c) {
                            mask2 |= sixel_bit(r);
                        }
                    }
                    if (mask2 != mask) break;
                    run++;
                }

                if (run >= 4) {
                    printf("!%d", run);
                    putchar(ch);
                } else {
                    for (int k = 0; k < run; k++) putchar(ch);
                }
                x += run;
            }
        }

        if (band + 1 < NUM_BANDS) putchar('-');
    }

    putchar(27);
    putchar('\\');
}

int main(void) {
    IO_PULSE_CLEAR(077);     /* NIOC 077 -- INTDS, disable interrupts */
    IO_PULSE_PULSE(FPU_DEV); /* NIOP 054 -- reset the FPU */
    IO_PULSE_START(FPU_DEV); /* NIOS 054 -- set the FPU to busy */
    IO_PULSE_START(FPU_AP1); /* NIOS 055 -- set the FPU_DMA to busy */
    load_psm(load_addr, FPU_PRGM_LEN, fpu_prgm);
    fps_run(load_addr);
    /* NOTE: unlike dma13.c, this test doesn't load/run an FPU
     * microcode program first (dma13.c's own fpu_prgm computes
     * Fibonacci via floating point -- unrelated to staging a 200x120
     * pixel-index image, so it was dropped rather than carried over
     * broken). Whatever actually populates the FPS-side 24000-word
     * image buffer before send_sixel_from_fps reads it out (a real
     * load_psm/fps_run pair with the right microcode, or some other
     * upstream source feeding the FPS) still needs to happen here in
     * a real deployment -- add that call above this one once that
     * program exists.
     */
    send_sixel_from_fps(results_addr);
    printf("\n");
    return 0;
}
