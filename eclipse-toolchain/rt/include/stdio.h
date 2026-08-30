#ifndef _ECLIPSE_STDIO_H
#define _ECLIPSE_STDIO_H

/* No stddef.h on this target -- see stdlib.h's identical NULL comment. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* Console-only stdio: there is no filesystem on this target, so no
 * FILE*, fopen/fclose, or freopen — only the console (TTI/TTO) exists as
 * an I/O device. See eclipse-toolchain/rt/eclipse_rt.c and
 * docs/IO_DEVICES.md (sibling eccc project) for the verified device
 * idiom these are built on.
 */

int putchar(int c);
int getchar(void);
int puts(const char *s);

/* Supports %d, %c, %s, %x, %u, %ld, %lu, %% — no field widths or
 * precision. %x/%u read a plain (16-bit) int/unsigned int; %ld/%lu
 * read a 32-bit long/unsigned long (this target's own "l" length
 * modifier, not promoted/truncated the way the others are). Every
 * other type is promoted to int/pointer per the usual C varargs
 * default-argument-promotion rules, which this target's uniformly-
 * 16-bit int/pointer sizes satisfy trivially — pass a genuinely
 * 16-bit-sized value to %u/%x (not a bare out-of-int-range literal
 * like 65535, which C promotes to long) or the va_arg reads
 * downstream desync.
 *
 * %x/%u/%l* each cost a few words of printf's own reachable-call-graph
 * weight (print_hex/print_long/print_uint32/u32_and_nz, all pulled in
 * unconditionally the moment any program calls printf at all, whether
 * it uses these specifiers or not) — same shared 256-word page-zero
 * budget %f was kept out of printf for (see this file's next comment).
 * Confirmed empirically: a program combining atof() with even a bare
 * printf("%d", n) can tip over that budget where it fit before this
 * printf was extended, even though atof() and printf() individually
 * still fit fine — if that happens, replace the printf call with
 * putchar()/puts() (no extra weight) to get back under budget.
 */
int printf(const char *fmt, ...);

/* Same feature set as printf() above, minus %o/%x/%u/%l* (add them the
 * same way printf's own cases do it, if a future caller needs them),
 * plus one thing printf() doesn't have: an optional "0" flag + decimal
 * width on %d only (e.g. "%02d") for zero-padding to a minimum digit
 * count. Writes into `buf` (caller must size it -- no bound checking,
 * same as the standard) and NUL-terminates it, instead of writing to
 * the console. Returns the number of characters written, excluding the
 * terminating NUL, same as the standard.
 */
int sprintf(char *buf, const char *fmt, ...);

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
