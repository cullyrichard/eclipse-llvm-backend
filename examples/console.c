#include "console.h"

static int tti_enabled = 0;

/* DOAS + poll SKPDN, the outa()-equivalent idiom (eclipse_io.h's own
 * outa macro would do the identical thing; written out by hand here,
 * same as vulcan.c/dsk.c, so this file has zero #include dependency on
 * eclipse_io.h/rt at all -- a true standalone kernel primitive). */
void console_putchar(int c) {
    asm volatile(
        "DOAS %0,011\n\t"
        "console_out_wait%=:\n\t"
        "SKPDN 011\n\t"
        "JMP console_out_wait%=\n\t"
        :: "r"(c));
}

/* One-time NIOS 010 enable, then poll SKPDN + DIAC (clear-pulse read).
 * Exactly eclipse_rt.c's own getchar() sequence, confirmed by reading
 * that file directly before writing this one. */
int console_getchar(void) {
    if (!tti_enabled) {
        asm volatile("NIOS 010");
        tti_enabled = 1;
    }
    int c;
    asm volatile(
        "console_in_wait%=:\n\t"
        "SKPDN 010\n\t"
        "JMP console_in_wait%=\n\t"
        "DIAC %0,010\n\t"
        : "=r"(c));
    return c;
}
