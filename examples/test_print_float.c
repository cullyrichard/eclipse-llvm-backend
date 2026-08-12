/* print_float() prints a float fixed-point with 6 digits after the
 * decimal point, matching printf("%f", ...)'s default precision — but
 * it's a separate function, not wired into printf's "%f" (see
 * SOFT_FLOAT_NOTES.md's "Why print_float isn't wired into printf" for
 * why: doing that would make every program that calls printf at all,
 * float or not, pull in the entire soft-float runtime).
 */
#include <stdio.h>

int main(void) {
    print_float(3.0f);
    putchar('\n');       /* 3.000000 */

    print_float(0.5f);
    putchar('\n');       /* 0.500000 */

    print_float(-2.25f);
    putchar('\n');       /* -2.250000 */

    print_float(100000.0f);
    putchar('\n');       /* 100000.000000 */

    return 0;
}
