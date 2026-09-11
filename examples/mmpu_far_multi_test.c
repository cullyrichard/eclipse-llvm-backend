#include <stdio.h>
#include "mmpu.h"

int main() {
    /* Two distinct physical pages, plus a repeat read to check for
     * state leakage between mmpu_read_far/mmpu_write_far calls -- see
     * mmpu_far_test.c for the single-address version this extends. */
    mmpu_write_far(0300, 0700, 9999);
    int r1 = mmpu_read_far(0300, 0700);
    printf("r1=%d\n", r1);

    /* second call to make sure state doesn't leak between calls */
    mmpu_write_far(0044, 0033, 111);
    int r2 = mmpu_read_far(0044, 0033);
    printf("r2=%d\n", r2);

    /* re-read the first one again to confirm it's still intact */
    int r1b = mmpu_read_far(0300, 0700);
    printf("r1b=%d\n", r1b);
    return 0;
}
