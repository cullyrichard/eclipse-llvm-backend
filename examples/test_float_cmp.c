#include <stdio.h>

int main(void) {
    float a = 3.0f;
    float b = 2.0f;
    printf("%d\n", a > b ? 1 : 0);      /* 1 */
    printf("%d\n", a < b ? 1 : 0);      /* 0 */
    printf("%d\n", a == 3.0f ? 1 : 0);  /* 1 */
    printf("%d\n", a != b ? 1 : 0);     /* 1 */
    printf("%d\n", b >= 2.0f ? 1 : 0);  /* 1 */
    printf("%d\n", b <= 1.0f ? 1 : 0);  /* 0 */
    return 0;
}
