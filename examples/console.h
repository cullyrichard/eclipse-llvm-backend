#ifndef _CONSOLE_H
#define _CONSOLE_H

/* console.h -- minimal kernel-level console driver: raw single-character
 * TTI/TTO I/O only, no line discipline (no CR/LF translation, no
 * buffering/echo policy). Device codes (TTI = 010, TTO = 011) and the
 * exact Busy/Done polling idiom used here are the same ones already
 * proven in this project's interrupt-driven I/O work (~/dev/
 * interrupt_test*.s: `dev TTO = 011`, `DOAS 0,011`/`NIOC 011`) and in
 * eclipse-toolchain/rt/eclipse_rt.c's own putchar/getchar -- confirmed
 * by reading that file directly before writing this one, not assumed.
 *
 * Relationship to eclipse-toolchain/rt/eclipse_rt.c's putchar/getchar:
 * this is a DELIBERATE, small duplication, not an oversight -- see
 * BLOCKDEV_NOTES.md's "Console driver" section for the full reasoning.
 * In short: rt/'s putchar/getchar are part of the user-level C runtime
 * (eclipse_rt.c), always linked in whole by eclipse-cc alongside printf/
 * scanf/the soft-float runtime/etc., and rt's putchar additionally does
 * a CR/LF line-discipline translation (`'\n'` -> `'\r'` + `'\n'`) that a
 * kernel-level raw driver should not bake in -- a kernel wants the raw
 * primitive so IT can decide the line discipline (or have none), the
 * same layering a real OS keeps between a UART driver and a tty layer
 * above it. console.c has zero dependency on eclipse_rt.c/stdio.h and
 * can be linked into kernel code on its own.
 */

#define TTI_DEVICE 010
#define TTO_DEVICE 011

/* console_putchar: send one character out TTO, polling until Done.
 * Blocking, matches every other device driver in this project's own
 * polled-completion convention (see blockdev.h's own scope note). */
void console_putchar(int c);

/* console_getchar: block until TTI has a character ready, then return
 * it (clear-pulse read, so a second call doesn't return stale data).
 * TTI needs one one-time enabling NIOS pulse before it responds to
 * anything at all (confirmed empirically already, by eclipse_rt.c's own
 * getchar -- see its header comment); this driver keeps that same
 * one-shot enable state internally, the same way eclipse_rt.c's own
 * tti_enabled flag does. */
int console_getchar(void);

#endif
