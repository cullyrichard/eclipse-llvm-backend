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

/* Prints `f` fixed-point with 6 digits after the decimal point (no
 * exponent notation, no NaN/Inf), followed by nothing (no trailing
 * newline) — same rounding/precision behavior as printf("%f", f), but
 * NOT reachable through printf() itself: printf() is called by nearly
 * every program on this target, and wiring float formatting into its
 * '%f' case would make the *entire* soft-float runtime (eclipse_rt.c's
 * "soft float" section — sf_add, __mulsf3, __fixsfsi, ...) statically
 * reachable from any program that calls printf at all, whether it uses
 * %f or not, permanently consuming this target's shared 256-word
 * page-zero budget. Confirmed empirically: even printf("%d\n", 42)
 * failed to assemble once %f lived inside printf's switch. Call this
 * directly instead: print_float(3.5f) rather than printf("%f", 3.5f).
 * No extra opt-in needed beyond just calling it — non-float programs
 * that never call print_float still pay nothing for its existence.
 * Returns nothing (unlike printf, no character count) — dropping that
 * bookkeeping was part of what got this under the ±127-word per-
 * function frame limit; see print_float's own comment in eclipse_rt.c.
 */
void print_float(float f);

#endif
