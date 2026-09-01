/* Verifies the optional --hwfloat feature: real Eclipse S/140 hardware
 * FPU instructions (FAS/FSS) computing float add/subtract, instead of
 * the default software __addsf3/__subsf3. See DEBUGGING_NOTES.md and
 * rt/eclipse_hwfloat.s for the format/calling-convention background.
 *
 * Compile with: eclipse-cc --hwfloat -o out.simh test_hwfloat.c
 * (Compiling *without* --hwfloat runs the exact same source through the
 * unmodified default software path -- same expected output either way,
 * confirmed identical on eclipseemu.)
 */
#include "stdio.h"

int main(void) {
  /* Whole numbers, positive */
  float a = 12.5f;
  float b = 4.25f;
  print_float(a + b);   /* 16.75 */
  putchar('\n');
  print_float(a - b);   /* 8.25 */
  putchar('\n');

  /* Negative operands */
  float c = -3.5f;
  float d = 2.0f;
  print_float(c + d);   /* -1.5 */
  putchar('\n');
  print_float(c - d);   /* -5.5 */
  putchar('\n');

  /* Larger magnitudes, subtraction crossing to negative */
  print_float(100.0f - 999.0f);  /* -899.0 */
  putchar('\n');

  /* Zero */
  print_float(0.0f + 0.0f);      /* 0.0 */
  putchar('\n');
  print_float(-5.0f - -5.0f);    /* 0.0 */
  putchar('\n');

  /* Fractional values */
  print_float(0.0625f + 0.0625f); /* 0.125 */
  putchar('\n');
  print_float(1.0f / 4.0f - 1.0f / 8.0f); /* 0.125 (uses software divide,
                                              then hardware subtract) */
  putchar('\n');

  /* Same operands reused across two different calls -- regression test
   * for a real bug found here: this target's calling convention pushes
   * call arguments *right-to-left*, and the first version of
   * __subsf3_hw got that backwards, silently computing b-a instead of
   * a-b whenever both operands were genuine variables (not constants
   * the frontend could constant-fold away) -- see DEBUGGING_NOTES.md.
   */
  float x = 12.5f;
  float y = 4.25f;
  print_float(x + y);  /* 16.75 */
  putchar('\n');
  print_float(x - y);  /* 8.25, not -8.25 */
  putchar('\n');

  return 0;
}
