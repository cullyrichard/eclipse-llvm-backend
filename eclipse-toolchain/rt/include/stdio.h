#ifndef _ECLIPSE_STDIO_H
#define _ECLIPSE_STDIO_H

/* Console-only stdio: there is no filesystem on this target, so no
 * FILE*, fopen/fclose, or freopen — only the console (TTI/TTO) exists as
 * an I/O device. See eclipse-toolchain/rt/eclipse_rt.c and
 * docs/IO_DEVICES.md (sibling eccc project) for the verified device
 * idiom these are built on.
 */

int putchar(int c);
int getchar(void);
int puts(const char *s);

/* Supports %d, %c, %s, %% only — no field widths, precision, or length
 * modifiers. Every other type is promoted to int/pointer per the usual
 * C varargs default-argument-promotion rules, which this target's
 * uniformly-16-bit int/pointer sizes satisfy trivially.
 */
int printf(const char *fmt, ...);

/* Supports %d, %c, %s only. Like eccc's own scanf (docs/LIMITATIONS.md,
 * sibling eccc project), literal characters in the format string other
 * than %-specifiers are skipped, not matched against input, and the
 * whitespace/delimiter that ends a %d or %s read is consumed rather than
 * left for the next call — fine for space/newline-separated input, not
 * for a format like "%d,%d" where the comma would be silently dropped.
 */
int scanf(const char *fmt, ...);

#endif
