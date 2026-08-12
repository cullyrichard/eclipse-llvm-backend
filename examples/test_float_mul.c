#include <stdio.h>

int main(void) {
    float a = 2.5f;
    float b = 4.0f;
    printf("%d\n", (int)(a * b));      /* 10 */

    float c = -3.0f;
    float d = 2.0f;
    printf("%d\n", (int)(c * d));      /* -6 */

    float e = 100000.0f;
    float f = 0.001f;
    printf("%d\n", (int)(e * f));      /* 100 */

    return 0;
}
