#include <stdio.h>

int main(void) {
    printf("%o\n", 8);     // expect "10"
    printf("%o\n", 511);   // expect "777"
    printf("%o\n", 0);     // expect "0"
    return 0;
}
