#include "console.h"

/* console_test.c -- real, no-stdio-dependency exercise of console.h.
 * Part 1: write a known marker string out via console_putchar only
 * (verified against SIMH's own captured terminal transcript -- the
 * string must appear literally in the run's stdout). Part 2: read a
 * known number of characters back via console_getchar and echo each
 * one straight back out via console_putchar -- verified by SIMH's SEND
 * command injecting those exact characters into the simulated TTI
 * device as if a user had typed them, then checking the echoed text in
 * the same captured transcript. See BLOCKDEV_NOTES.md for the real
 * transcript and the SEND invocation used.
 *
 * No <stdio.h>, no eclipse_rt.c dependency -- this program only ever
 * calls console_putchar/console_getchar, proving console.c really is a
 * standalone kernel-level primitive, not secretly leaning on rt's own
 * putchar/getchar.
 */

static void console_puts(const char *s) {
    while (*s) {
        console_putchar(*s);
        s++;
    }
}

int main(void) {
    int i;

    console_puts("CONSOLE-OUT-OK\r\n");

    for (i = 0; i < 5; i++) {
        int c = console_getchar();
        console_putchar(c);
    }

    console_puts("\r\nECHO-DONE\r\n");

    return 0;
}
