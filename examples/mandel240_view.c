#include <stdio.h>
#include <eclipse_io.h>
#include "fps.h"
#include "MANDEL240.h"  /* PROG_ps[340], PROG_PS_ADDR=0, PROG_PS_SIZE=340 */

/* Eclipse S/140 host driver for the FPS-100/AP-120B MANDEL240 program:
 * loads it, runs it in 6 batches (per MANDEL240_TRANSFER.md), and
 * streams the resulting 400x240 iteration-count image out as a SIXEL
 * image over the serial console, one 6-row band at a time.
 *
 * REAL, UNVERIFIED ASSUMPTIONS -- flagged clearly rather than papered
 * over, since none of this can be checked on eclipseemu (it doesn't
 * simulate device 054 at all) and none of it is confirmed anywhere
 * else in this project. If the image comes out wrong, these are the
 * first three things to check, in order of likelihood:
 *
 * 1. RESUME-AFTER-HALT (see resume_ap() below): assumed to be "re-issue
 *    fn_start without reloading TMA" -- i.e. the AP's own microsequencer
 *    PC survives a HALT, and a bare Start resumes it in place; TMA is
 *    only meaningful for the very first start (which jumps to a
 *    specific address). This is the common convention for a microcode
 *    sequencer's HALT/continue pair, but it is NOT confirmed on this
 *    hardware. If batch 1 onward repeats batch 0's image data (i.e.
 *    the bottom half of the image mirrors the top instead of
 *    continuing it), this assumption is wrong -- try reading
 *    fn_examine_regpsa (already defined in fps.h, never used yet) via
 *    cmd_rdlt to get the AP's actual current PC, and reload TMA with
 *    that value before each fn_start instead of skipping TMA entirely.
 *
 * 2. NOT OVERLAPPED/PIPELINED: MANDEL240_TRANSFER.md describes draining
 *    batch N's DMA while the AP computes batch N+1 -- this needs a
 *    non-blocking DMA start plus a separate "wait for DMA done"
 *    primitive (the notes' own WTDMA/APWD) that doesn't exist anywhere
 *    in this project's fps.c/fps.h. Rather than invent one unverified
 *    primitive on top of another, this version is fully sequential:
 *    wait for HALT, drain the whole batch via host_dma_in (assumed to
 *    block until the transfer is actually complete -- host_dma_in's own
 *    body has no visible completion-wait either, so this is itself an
 *    assumption, just a more conservative one), THEN resume the AP.
 *    Slower (no compute/DMA overlap), not a correctness risk. Revisit
 *    once DMA-completion signaling is confirmed on real hardware.
 *
 * 3. FMT=1's exact semantics (does host_dma_in's transfer already
 *    strip the AP-native MI<DB;DB=SPFN encoding down to a plain
 *    iteration count, or does the raw 3-word encoding leak through?)
 *    are UNCONFIRMED per MANDEL240_TRANSFER.md's own Sec.3. This code
 *    assumes each transferred word already IS the plain iteration
 *    count (0..32). If the image is noise/garbage rather than a
 *    recognizable Mandelbrot shape, this is the next thing to check --
 *    try decoding return_data[] the way read_md()/calculate_value() in
 *    this same file decode the FPU's *other* (non-DMA, register-poll)
 *    read path instead, in case FMT=1 hands back the raw triple.
 *
 * MEMORY: this target's logical address space is 32,768 words total --
 * a full 400x240 image (96,000 words) does not fit, not even close.
 * This is why the image is streamed one SIXEL band (6 image rows =
 * 2,400 words) at a time, DMA'd one image row (400 words) at a time
 * into a small rolling buffer, rather than assembled host-side first.
 * 40 rows/batch is not a multiple of 6, so a band can straddle a batch
 * boundary (e.g. band 6 = rows 36-41, spanning batch 0's last 4 rows
 * and batch 1's first 2) -- the per-row DMA loop below runs across
 * batch boundaries transparently (it just tracks an absolute row
 * counter and resumes the AP whenever it needs the next batch's data),
 * so no special-casing is needed for this.
 */

#define IMG_WIDTH        400
#define IMG_HEIGHT       240
#define ROWS_PER_BATCH   40
#define NUM_BATCHES      6
#define BAND_ROWS        6
#define MAXITER          32   /* iteration counts run 0..32 inclusive */

#define MD_HALF_0        0000000
#define MD_HALF_1        0040000

static unsigned int row_buf[IMG_WIDTH];

/* Flattened 1D, NOT a genuine `unsigned int band[BAND_ROWS][IMG_WIDTH]`
 * -- found the hard way, while building this driver, that this
 * backend's code generation for a real 2D array with BOTH indices
 * runtime-computed writes to the wrong address and silently corrupts
 * adjacent memory (confirmed via a minimal repro: `band[r][x] = v`
 * inside nested runtime loops overwrote bytes of an unrelated string
 * literal elsewhere in the program -- the corrupted byte's value
 * matched exactly what the loop was writing). The identical `r *
 * IMG_WIDTH + x` address arithmetic done manually against a flat 1D
 * array does NOT reproduce the bug -- only the compiler's own
 * multi-dimensional array indexing does. Flagged as a real, separate
 * backend bug worth fixing on its own; this workaround just avoids it.
 */
static unsigned int band[BAND_ROWS * IMG_WIDTH];
#define BAND(r, x) band[(r) * IMG_WIDTH + (x)]

/* Same protocol test_fps_add.c's original hand-assembled version used
 * (see fps.h's own header comment): poll cmd_rdfn via DIB until the
 * fn_stop bit is set. Bare polling, no timeout -- matches every other
 * use of this pattern in this project. */
static void wait_stop(void) {
    while (!(fpu_in(cmd_rdfn) & fn_stop)) {
        /* spin */
    }
}

/* First start: load TMA with the entry address, then start. */
static void start_ap(unsigned int addr) {
    fpu_out(cmd_wtsr, addr);
    fpu_out(cmd_wtfn, fn_load_tma);
    fpu_out(cmd_wtfn, fn_start);
}

/* Resume after a mid-program HALT -- see assumption #1 above. */
static void resume_ap(void) {
    fpu_out(cmd_wtfn, fn_start);
}

/* --- SIXEL output ---
 *
 * Palette: iteration counts 0..31 (a pixel that escaped) map to a
 * grayscale ramp, black (0) at the fastest escape up to white (31) at
 * the slowest -- brighter means "closer to the boundary." Iteration
 * count 32 (MAXITER, never escaped -- inside the set) gets its own
 * distinct blue, so the set's interior reads clearly against even the
 * brightest escaping pixels. Simple and easy to verify by hand; swap
 * for a fancier gradient later if wanted.
 *
 * SIXEL color registers use Pu=2 (RGB, each channel 0-100 percent) --
 * defined once up front, referenced by index (0..32) from then on.
 */
static void sixel_write_palette(void) {
    int i;
    for (i = 0; i <= 31; i++) {
        int pct = (i * 100) / 31;
        printf("#%d;2;%d;%d;%d", i, pct, pct, pct);
    }
    printf("#%d;2;0;0;100", MAXITER); /* inside-the-set blue */
}

/* Run-length-encode one color's 400-column sixel row within the
 * current band and emit it: `#<color>` selects the palette entry, then
 * `!<count><char>` repeats for runs of 3+ (SIXEL's own RLE -- shorter
 * runs are cheaper written out plain), `$` at the end returns to the
 * start of the band (not next band) so the next color's pass overlays
 * the same six rows. Columns not using `color` in this band are '?'
 * (sixel value 0 -- transparent/blank), which is exactly what
 * `bits_for_column` naturally produces for a non-matching pixel, so no
 * separate blanking pass is needed.
 */
/* `1 << r` for a runtime `r` is a variable-count shift -- this backend
 * only supports shifts by a compile-time constant amount (confirmed by
 * `llc` itself: "Eclipse backend only supports shifts by a
 * compile-time constant amount... no variable-count shift/rotate
 * hardware is exposed yet"). A lookup table sidesteps this instead of
 * unrolling the row loop by hand. */
static const int row_bit[BAND_ROWS] = {1, 2, 4, 8, 16, 32};

static int column_bits(int x, int color) {
    int bits = 0;
    int r;
    for (r = 0; r < BAND_ROWS; r++) {
        if (BAND(r, x) == (unsigned int)color) {
            bits |= row_bit[r];
        }
    }
    return bits;
}

static void sixel_emit_color_pass(int color) {
    int x;
    int any = 0;
    printf("#%d", color);
    for (x = 0; x < IMG_WIDTH; ) {
        int bits = column_bits(x, color);
        char ch = (char)(0x3F + bits);
        if (bits != 0) any = 1;

        int run = 1;
        while (x + run < IMG_WIDTH) {
            if (column_bits(x + run, color) != bits) {
                break;
            }
            run++;
        }

        if (run >= 3) {
            printf("!%d", run);
            putchar(ch);
        } else {
            int k;
            for (k = 0; k < run; k++) {
                putchar(ch);
            }
        }
        x += run;
    }
    if (!any) {
        /* Nothing of this color in this band -- the pass above still
         * emitted a full row of '?' (harmless, just wasted bytes) since
         * `bits` is 0 for every column when `any` stays 0. Rare enough
         * (a color genuinely absent from a whole 6-row band) that
         * skipping the pass isn't worth the extra bookkeeping here;
         * left as a possible follow-up if serial transfer time matters
         * more than code simplicity. */
    }
    printf("$");
}

static void sixel_emit_band(void) {
    int color;
    for (color = 0; color <= MAXITER; color++) {
        sixel_emit_color_pass(color);
    }
    printf("-");
}

int main(void) {
    IO_PULSE_CLEAR(077);       /* NIOC 077 -- INTDS, disable interrupts */
    IO_PULSE_PULSE(FPU_DEV);   /* NIOP 054 -- reset the FPU */
    IO_PULSE_START(FPU_DEV);   /* NIOS 054 -- set the FPU to busy */
    IO_PULSE_START(FPU_AP1);   /* NIOS 055 -- set the FPU_DMA to busy */

    /* PROG_ps is uint16_t[] (from ps_md_to_c.py's generated header);
     * load_psm wants unsigned int[] -- identical width on this target
     * (both 16 bits), just a distinct C type, so a plain reinterpret
     * cast is safe here. */
    load_psm(PROG_PS_ADDR, PROG_PS_SIZE, (const unsigned int *)PROG_ps);

    printf("\x1bPq");
    sixel_write_palette();

    int abs_row = 0;
    int band_row = 0;
    int batch;

    for (batch = 0; batch < NUM_BATCHES; batch++) {
        if (batch == 0) {
            start_ap(PROG_PS_ADDR);
        } else {
            resume_ap();
        }
        wait_stop();

        unsigned int half_base = (batch & 1) ? MD_HALF_1 : MD_HALF_0;
        int r;
        for (r = 0; r < ROWS_PER_BATCH; r++) {
            unsigned int md_addr = half_base + 1 + (unsigned int)r * IMG_WIDTH;
            host_dma_in((unsigned int)row_buf, IMG_WIDTH, md_addr);

            int x;
            for (x = 0; x < IMG_WIDTH; x++) {
                BAND(band_row, x) = row_buf[x];
            }
            band_row++;
            abs_row++;

            if (band_row == BAND_ROWS) {
                sixel_emit_band();
                band_row = 0;
            }
        }
    }

    printf("\x1b\\"); /* ST -- end sixel sequence */
    putchar('\n');

    return 0;
}
