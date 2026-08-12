#include <stdio.h>

int main(void) {
    float a = 10.0f;
    float b = 4.0f;
    printf("%d\n", (int)(a / b));      /* 2 (truncated 2.5) */

    float c = 7.0f;
    float d = 2.0f;
    printf("%d\n", (int)(c / d));      /* 3 (truncated 3.5) */

    float e = -9.0f;
    float f = 3.0f;
    printf("%d\n", (int)(e / f));      /* -3 */

    return 0;
}
