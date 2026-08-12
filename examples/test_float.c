/* Combined stress test exercising every soft-float capability at once
 * (all four arithmetic ops, all six comparisons, and conversions). Each
 * individual result below is correct and verified — but compiling this
 * many float functions into ONE program is expected to exceed this
 * target's shared 256-word page-zero budget (see SOFT_FLOAT_NOTES.md's
 * "Known limit" section) once eclipse-cc's retry logic has discovered
 * everything this file needs and tries to protect it all at once.
 *
 * For a working single-file smoke test, use one of test_float_mul.c /
 * test_float_div.c / test_float_cmp.c / test_float_conv.c instead — each
 * exercises one capability and fits comfortably within budget.
 */
#include <stdio.h>

int main(void) {
    float a = 3.0f;
    float b = 2.0f;

    printf("%d\n", (int)(a + b));      /* 5 */
    printf("%d\n", (int)(a - b));      /* 1 */
    printf("%d\n", (int)(a * b));      /* 6 */
    printf("%d\n", (int)(a / b));      /* 1 (truncated 1.5) */

    float c = 2.5f;
    float d = 4.0f;
    printf("%d\n", (int)(c * d));      /* 10 */

    float e = -3.0f;
    float f = 5.0f;
    printf("%d\n", (int)(e + f));      /* 2 */

    float i = 7.0f;
    float j = 2.0f;
    printf("%d\n", (int)(i / j));      /* 3 (truncated 3.5) */

    int k = 7;
    float kf = (float)k;
    printf("%d\n", (int)kf);           /* 7 */

    printf("%d\n", a > b ? 1 : 0);     /* 1 */
    printf("%d\n", a < b ? 1 : 0);     /* 0 */
    printf("%d\n", a == 3.0f ? 1 : 0); /* 1 */
    printf("%d\n", a != b ? 1 : 0);    /* 1 */
    printf("%d\n", b >= 2.0f ? 1 : 0); /* 1 */
    printf("%d\n", b <= 1.0f ? 1 : 0); /* 0 */

    float big = 12345.0f;
    printf("%d\n", (int)big);          /* 12345 */

    return 0;
}
