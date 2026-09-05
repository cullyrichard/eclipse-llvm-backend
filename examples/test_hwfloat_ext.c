#include <stdio.h>

/* Verifies the hardware-backed comparison and int<->float conversion
 * paths added alongside __addsf3_hw/__subsf3_hw (see rt/eclipse_hwfloat.s).
 * Every expected value below is independently computed, not eyeballed.
 * Compiled with the default (hardware) build, this exercises
 * __eqsf2_hw/__nesf2_hw/__ltsf2_hw/__lesf2_hw/__gtsf2_hw/__gesf2_hw and
 * __floatsisf_hw/__floatunsisf_hw/__fixsfsi_hw/__fixunssfsi_hw,
 * including cases designed to fall through their software fallback
 * (values outside the hardware-safe range). Compiled with --ieee, this
 * exercises the exact same original software paths and must produce
 * byte-identical output.
 */
int main(void) {
  float a = 3.0f, b = 2.0f, c = 3.0f;
  float negv = -5.0f, zero1 = 0.0f, zero2 = 0.0f;
  float frac1 = 0.125f, frac2 = 0.25f;

  printf("%d\n", a > b ? 1 : 0);          /* 1 */
  printf("%d\n", a < b ? 1 : 0);          /* 0 */
  printf("%d\n", a == c ? 1 : 0);         /* 1 */
  printf("%d\n", a != b ? 1 : 0);         /* 1 */
  printf("%d\n", b >= 2.0f ? 1 : 0);      /* 1 */
  printf("%d\n", b <= 1.0f ? 1 : 0);      /* 0 */
  printf("%d\n", negv < zero1 ? 1 : 0);   /* 1 */
  printf("%d\n", negv > zero1 ? 1 : 0);   /* 0 */
  printf("%d\n", zero1 == zero2 ? 1 : 0); /* 1 */
  printf("%d\n", frac1 < frac2 ? 1 : 0);  /* 1 */
  printf("%d\n", negv == -5.0f ? 1 : 0);  /* 1 */
  printf("%d\n", negv != -5.0f ? 1 : 0);  /* 0 */
  printf("%d\n", b == a ? 1 : 0);         /* 0 */
  printf("%d\n", negv <= -5.0f ? 1 : 0);  /* 1 */
  printf("%d\n", negv >= -4.0f ? 1 : 0);  /* 0 */

  {
    int ip = 42;
    int ineg = -17;
    unsigned int up = 100;
    long lbig = 100000L;
    long lnegbig = -100000L;
    unsigned long ubig = 70000UL;
    int izero = 0;

    printf("%d\n", (int)(float)ip);              /* 42 */
    printf("%d\n", (int)(float)ineg);             /* -17 */
    printf("%d\n", (int)(float)up);               /* 100 */
    printf("%ld\n", (long)(float)lbig);           /* 100000 */
    printf("%ld\n", (long)(float)lnegbig);        /* -100000 */
    printf("%lu\n", (unsigned long)(float)ubig);  /* 70000 */
    printf("%d\n", (int)(float)izero);            /* 0 */
  }

  {
    float f1 = 42.9f;
    float f2 = -17.9f;
    float f3 = 100.9f;
    float f4 = 0.0f;
    float f5 = -32768.0f;
    float f6 = 32767.9f;
    float fbig = 100000.5f;
    float fnegbig = -100000.5f;

    printf("%d\n", (int)f1);              /* 42 */
    printf("%d\n", (int)f2);              /* -17 */
    printf("%u\n", (unsigned int)f3);     /* 100 */
    printf("%d\n", (int)f4);              /* 0 */
    printf("%ld\n", (long)f5);            /* -32768 */
    printf("%d\n", (int)f6);              /* 32767 */
    printf("%ld\n", (long)fbig);          /* 100000 */
    printf("%ld\n", (long)fnegbig);       /* -100000 */
  }

  return 0;
}
