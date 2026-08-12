#ifndef _ECLIPSE_STDLIB_H
#define _ECLIPSE_STDLIB_H

int atoi(const char *s);

/* Bump allocator over a fixed static arena (no OS heap to draw from —
 * see eclipse_rt.c). free() is a real, callable no-op: memory is never
 * reclaimed. Fine for programs with modest, bounded allocation; a
 * long-running program that allocates in a loop will exhaust the arena.
 * A real free-list allocator is future work if that turns out to matter
 * in practice.
 */
void *malloc(unsigned int size);
void free(void *ptr);

#endif
