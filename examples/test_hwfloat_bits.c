#include <stdio.h>

/* Follow-up to test_hwfloat_minimal.c: on real hardware, v + 1.0f (via
 * the hardware FAS add) printed as -2147483648.000000 instead of
 * 9.000000. -2147483648 is exactly -2^31, a classic 32-bit signed-
 * overflow wraparound pattern -- so before concluding FAS itself
 * produced garbage, get the RAW bits of the result directly (bypassing
 * print_float's own digit-extraction, which could itself overflow/wrap
 * when handed a genuinely huge-magnitude float and display something
 * misleading, independent of whether the underlying value is actually
 * corrupt). This project's own established discipline: verify at the
 * bit level, don't trust a higher-level print routine's rendering.
 */

static volatile unsigned int g_hi = 16640; /* 0x4100 -> 8.0 */
static volatile unsigned int g_lo = 0;

static float bits_to_float(unsigned int hi, unsigned int lo) {
    union {
        unsigned long u;
        float f;
    } v;
    v.u = ((unsigned long)hi << 16) | (unsigned long)lo;
    return v.f;
}

static void dump_float_bits(const char *label, float f) {
    union {
        unsigned long u;
        float f;
    } v;
    v.f = f;
    printf("%s: bits hi=%u lo=%u (hex hi=%x lo=%x)\n", label,
           (unsigned int)(v.u >> 16), (unsigned int)(v.u & 0xFFFFUL),
           (unsigned int)(v.u >> 16), (unsigned int)(v.u & 0xFFFFUL));
}

int main(void) {
    float v, w;

    printf("A: entered\n");

    v = bits_to_float(g_hi, g_lo);
    dump_float_bits("B: v", v);

    w = v + 1.0f; /* hardware ADD (FAS) */
    dump_float_bits("C: w", w);

    printf("DONE\n");
    return 0;
}
