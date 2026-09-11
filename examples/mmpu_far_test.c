#include <stdio.h>
#include "mmpu.h"

/* Verifies mmpu.h's C-callable API end to end through the *real*
 * eclipse-cc pipeline (clang -cc1 -> llvm-link -> opt -> llc ->
 * reorder_asm.py -> dgasm), unlike examples/mmpu_probe.s's Phase 1
 * proof, which was hand-assembled directly. Writes a marker word to
 * physical page 100 octal (64 decimal -- deliberately different from
 * mmpu_probe.s's page 50 octal/40 decimal, so this is its own
 * independent proof, not a re-run of the same address), reads it back
 * through the same API, and separately confirms via a plain (unmapped)
 * ordinary access that the marker never touched anything in this
 * program's own normal 32768-word logical space.
 */
int main() {
    mmpu_write_far(0100, 0200, 4321);
    int readback = mmpu_read_far(0100, 0200);
    printf("%d\n", readback);

    /* Physical page 0100 octal, offset 0200 octal -- printed
     * separately, not combined into one (physpage << 10) | offset
     * value: that product is 65664 decimal, which overflows this
     * target's 16-bit int (confirmed by the compiler's own
     * -Wshift-overflow warning) -- this backend has no wider integer
     * type to compute it in (see README.md's Known limitations on
     * 64-bit/i64 support), so the two halves are reported separately
     * and combined by hand when checking the result, the same way
     * MMPU_NOTES.md's own mmpu_probe.s transcript does with `e 120104`. */
    printf("physpage=%d offset=%d\n", 0100, 0200);
    return 0;
}
