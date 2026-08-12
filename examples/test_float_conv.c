#include <stdio.h>

int main(void) {
    int k = 42;
    float kf = (float)k;
    printf("%d\n", (int)kf);           /* 42 */

    unsigned int u = 100;
    float uf = (float)u;
    printf("%d\n", (int)uf);           /* 100 */

    int neg = -17;
    float negf = (float)neg;
    printf("%d\n", (int)negf);         /* -17 */

    return 0;
}
