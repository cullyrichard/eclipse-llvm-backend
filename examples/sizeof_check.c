#include <stdio.h>

struct pair {
    int a;
    int b;
};

int global_arr[5];

int main(void) {
    printf("%d\n", (int)sizeof(char));
    printf("%d\n", (int)sizeof(short));
    printf("%d\n", (int)sizeof(int));
    printf("%d\n", (int)sizeof(long));
    printf("%d\n", (int)sizeof(int *));
    printf("%d\n", (int)sizeof(struct pair));
    printf("%d\n", (int)sizeof(global_arr));
    printf("%d\n", (int)sizeof(int[7]));
    return 0;
}
