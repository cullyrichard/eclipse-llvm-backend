#ifndef _ECLIPSE_STDLIB_H
#define _ECLIPSE_STDLIB_H

/* No stddef.h on this target -- NULL is defined redundantly (guarded)
 * in every header a standard C program might reasonably expect it
 * from, matching how a real libc's stddef.h ends up pulled in
 * transitively by several different headers.
 */
#ifndef NULL
#define NULL ((void *)0)
#endif

int atoi(const char *s);
float atof(const char *s);
long strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);

int abs(int n);
long labs(long n);

void srand(unsigned int seed);
int rand(void);

/* HALT immediately (see eclipse_rt.c) -- status is not surfaced
 * anywhere (no OS, no exit-code consumer), so it's accepted but
 * ignored, matching this target's existing exit-at-end-of-main
 * behavior (EclipseAsmPrinter's _start emits HALT right after the
 * JSR to main anyway -- exit() just makes that reachable early too).
 */
void exit(int status) __attribute__((noreturn));

/* Bump allocator over a fixed static arena (no OS heap to draw from —
 * see eclipse_rt.c). free() is a real, callable no-op: memory is never
 * reclaimed. Fine for programs with modest, bounded allocation; a
 * long-running program that allocates in a loop will exhaust the arena.
 * A real free-list allocator is future work if that turns out to matter
 * in practice.
 */
void *malloc(unsigned int size);
void free(void *ptr);

/* calloc(nmemb, size) == malloc(nmemb*size) + zero-fill. nmemb/size are
 * plain unsigned int (16-bit) per the standard signature; see
 * eclipse_rt.c's implementation for why that multiply is deliberately
 * kept 16-bit-native rather than promoted anywhere.
 */
void *calloc(unsigned int nmemb, unsigned int size);

#endif
