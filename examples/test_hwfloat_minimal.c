#include <stdio.h>

/* Isolates each individual hardware-FPU operation class with its own
 * checkpoint, to find out exactly which one hangs on REAL hardware --
 * mandel240_diag5.c's PASS2 hung immediately (before even its first
 * i=49 checkpoint) on data that, run through the IDENTICAL computation
 * on eclipseemu, completed instantly. Since bits_to_float() itself is a
 * plain union bit-reinterpret (no FPU instruction at all -- just LDA/
 * STA), the hang has to be in the hardware add (FAS, entry #27),
 * compare (FCMP, entry #31), or float->int convert (FFAS, entry #31)
 * -- none of which, as far as this project's own history shows, have
 * actually been exercised on the real physical Eclipse before now (only
 * ever verified against eclipseemu's simulation of those instructions).
 *
 * volatile globals prevent the compiler from constant-folding the
 * whole chain away at compile time (which it otherwise could, since
 * every value here is technically known at compile time).
 */

static volatile unsigned int g_hi = 16640; /* 0x4100 */
static volatile unsigned int g_lo = 0;

static float bits_to_float(unsigned int hi, unsigned int lo) {
    union {
        unsigned long u;
        float f;
    } v;
    v.u = ((unsigned long)hi << 16) | (unsigned long)lo;
    return v.f;
}

int main(void) {
    float v, w;
    int cmp, iv;

    printf("A: entered\n");

    v = bits_to_float(g_hi, g_lo); /* no FPU instruction -- plain bit reinterpret */
    printf("B: decoded v=");
    print_float(v);
    putchar('\n');

    w = v + 1.0f; /* hardware ADD (FAS) -- entry #27, previously the most-used hw op */
    printf("C: added w=");
    print_float(w);
    putchar('\n');

    cmp = (v < w); /* hardware COMPARE (FCMP + FSS-based diff) -- entry #31 */
    printf("D: compared cmp=%d\n", cmp);

    iv = (int)v; /* hardware float->int CONVERT (FFAS) -- entry #31 */
    printf("E: converted iv=%d\n", iv);

    printf("DONE\n");
    return 0;
}
