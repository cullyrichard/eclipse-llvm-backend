#include "eclipse_rt.h"
#include <stdarg.h>

/* Console device codes (docs/ECLIPSE_ISA_NOTES.md): TTI = 010, TTO = 011. */

static int tti_enabled = 0;

/* out(TTO, c): DOAS starts the output pulse with c in an accumulator,
 * then poll SKPDN until the Done flag is set. DOAS must come before the
 * poll loop, or the loop spins forever (nothing ever sets Done) — see
 * docs/ECLIPSE_ISA_NOTES.md's "Order matters" note. Confirmed empirically
 * against eclipseemu (real 'A' printed) for this exact backend/idiom.
 *
 * The label must be alone on its own line: dgasm's grammar is
 * `label_stmt: IDENTIFIER COLON EOL`, so `label: INSTR` on one line is a
 * parse error.
 */
static void putraw(int c) {
  asm volatile(
      "DOAS %0,011\n\t"
      "wait%=:\n\t"
      "SKPDN 011\n\t"
      "JMP wait%=\n\t"
      :: "r"(c));
}

/* Real Eclipse hardware terminals don't auto-return to column 0 on a bare
 * LF the way eclipseemu's host-terminal pty does — without an explicit CR
 * first, each '\n' drops the cursor a line without resetting its column,
 * so successive lines drift one line's worth further right each time
 * ("ever growing spaces" staircase). Confirmed on real hardware, not
 * reproducible in eclipseemu since the simulator's terminal already does
 * LF->CRLF translation itself. Fixed here, at the single lowest common
 * point every other newline-emitting call (printf's literal '\n' chars,
 * %s strings containing '\n', print_string's trailing putchar('\n'))
 * already routes through, rather than at each call site individually.
 */
int putchar(int c) {
  if (c == '\n')
    putraw('\r');
  putraw(c);
  return c;
}

/* in(TTI): a device generally needs an enabling NIOS pulse before it
 * responds to anything else (confirmed empirically: polling SKPDN TTI
 * before ever issuing NIOS TTI never saw Done set — docs/IO_DEVICES.md).
 * Read with DIAC (clear-pulse variant), not plain DIA — DIA left the
 * Done flag set, so a second read silently returned the same stale
 * character instead of waiting for a new one (docs/IO_DEVICES.md).
 */
int getchar(void) {
  if (!tti_enabled) {
    asm volatile("NIOS 010");
    tti_enabled = 1;
  }
  int c;
  asm volatile(
      "wait%=:\n\t"
      "SKPDN 010\n\t"
      "JMP wait%=\n\t"
      "DIAC %0,010\n\t"
      : "=r"(c));
  return c;
}

int puts(const char *s) {
  int n = 0;
  while (*s) {
    putchar(*s);
    s++;
    n++;
  }
  putchar('\n');
  return n + 1;
}

static int print_uint(unsigned int val) {
  int n = 0;
  if (val >= 10) {
    n += print_uint(val / 10);
  }
  putchar('0' + (val % 10));
  return n + 1;
}

static int print_int(int val) {
  int n = 0;
  if (val < 0) {
    putchar('-');
    n++;
    val = -val;
  }
  return n + print_uint((unsigned int)val);
}

static int print_octal(unsigned int val) {
  int n = 0;
  if (val >= 8) {
    n += print_octal(val / 8);
  }
  putchar('0' + (val % 8));
  return n + 1;
}

/* Forward declarations: the printf %lx/%lu/%ld helpers below need two
 * things from the soft-float section, much further down this file --
 * print_uint32 (decimal formatting for a real 32-bit value) and
 * u32_and_nz (this target's own workaround for 32-bit comparisons used
 * as a branch condition: writing that comparison directly, e.g.
 * val > 0, crashes llc with a 'Cannot select' error -- see that
 * section's own comment on print_long below, and sf_add_extract's use
 * of the same pattern, for the full story).
 */
static int u32_and_nz(unsigned long a, unsigned long b);
static int print_uint32(unsigned long val);

static const char print_hex_digits[] = "0123456789abcdef";

static int print_hex(unsigned int val) {
  int n = 0;
  if (val >= 16) {
    n += print_hex(val >> 4);
  }
  putchar(print_hex_digits[val & 15]);
  return n + 1;
}

/* Sign handled via a bitmask test (u32_and_nz against the sign bit),
 * not a plain "val < 0" comparison -- a 32-bit signed comparison hits
 * the same ISel crash a 32-bit unsigned one does (see the forward-
 * declaration comment above). Matches sf_add_extract's
 * u32_and_nz(a, SF_SIGN_MASK) idiom exactly.
 */
static int print_long(long val) {
  int n = 0;
  unsigned long uval = (unsigned long)val;
  if (u32_and_nz(uval, 0x80000000UL)) {
    putchar('-');
    n++;
    uval = (unsigned long)(-val);
  }
  return n + print_uint32(uval);
}

int printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = 0;
  while (*fmt) {
    if (*fmt == '%') {
      fmt++;
      if (*fmt == 'd' || *fmt == 'i') {
        // %i is a synonym for %d in printf (unlike scanf, where they
        // differ) -- added after c-testsuite 00210 silently printed a
        // bare newline (no digits) for "%i\n": the missing case fell
        // through this if-chain untaken, so *fmt never advanced past
        // 'i' via a matched branch and no digits were ever emitted,
        // but the format string's own literal '\n' right after still
        // printed normally -- easy to misread as "no output at all"
        // since eclipse-toolchain's own test harness strips exactly one
        // blank-line/banner pair and can't tell that blank line apart
        // from a real one-character printf output.
        n += print_int(va_arg(ap, int));
      } else if (*fmt == 'o') {
        n += print_octal((unsigned int)va_arg(ap, int));
      } else if (*fmt == 'c') {
        putchar(va_arg(ap, int));
        n++;
      } else if (*fmt == 's') {
        const char *s = va_arg(ap, const char *);
        while (*s) {
          putchar(*s);
          s++;
          n++;
        }
      } else if (*fmt == 'x') {
        n += print_hex((unsigned int)va_arg(ap, int));
      } else if (*fmt == 'u') {
        n += print_uint((unsigned int)va_arg(ap, int));
      } else if (*fmt == 'l') {
        fmt++;
        if (*fmt == 'd') {
          n += print_long(va_arg(ap, long));
        } else if (*fmt == 'u') {
          n += print_uint32((unsigned long)va_arg(ap, long));
        }
      } else if (*fmt == '%') {
        putchar('%');
        n++;
      }
      if (*fmt) {
        fmt++;
      }
    } else {
      putchar(*fmt);
      n++;
      fmt++;
    }
  }
  va_end(ap);
  return n;
}

/* sprintf: like printf but writes into a caller-supplied buffer and
 * NUL-terminates it, instead of writing to the console. Deliberately
 * NOT layered on printf()'s existing helpers (print_int/print_uint/...):
 * every one of those calls putchar() directly with no output-sink
 * indirection, so retrofitting a buffer target into all of them would
 * touch printf's own hot path for no benefit here -- an independent,
 * small formatter is simpler and lower-risk. Supports %d (with an
 * optional "0"-flag + decimal width, e.g. %02d, for zero-padding -- the
 * one field-width feature anything on this target has needed so far),
 * %c, %s, %% -- no %o/%x/%u/%l* yet; add them the same way (mirroring
 * printf's own cases) if a future caller needs them.
 */
static char *sprintf_put(char *out, int c) {
  *out = (char)c;
  return out + 1;
}

static char *sprintf_uint(char *out, unsigned int val) {
  if (val >= 10) {
    out = sprintf_uint(out, val / 10);
  }
  return sprintf_put(out, '0' + (val % 10));
}

static int sprintf_udigits10(unsigned int val) {
  int n = 1;
  while (val >= 10) {
    val /= 10;
    n++;
  }
  return n;
}

int sprintf(char *buf, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *out = buf;
  while (*fmt) {
    if (*fmt == '%') {
      fmt++;
      int zero_pad = 0;
      int width = 0;
      if (*fmt == '0') {
        zero_pad = 1;
        fmt++;
      }
      while (*fmt >= '0' && *fmt <= '9') {
        width = width * 10 + (*fmt - '0');
        fmt++;
      }
      if (*fmt == 'd') {
        int val = va_arg(ap, int);
        int neg = (val < 0);
        unsigned int uval = neg ? (unsigned int)(-val) : (unsigned int)val;
        int total = sprintf_udigits10(uval) + (neg ? 1 : 0);
        if (neg) {
          out = sprintf_put(out, '-');
        }
        while (total < width) {
          out = sprintf_put(out, zero_pad ? '0' : ' ');
          total++;
        }
        out = sprintf_uint(out, uval);
      } else if (*fmt == 'c') {
        out = sprintf_put(out, va_arg(ap, int));
      } else if (*fmt == 's') {
        const char *s = va_arg(ap, const char *);
        while (*s) {
          out = sprintf_put(out, *s);
          s++;
        }
      } else if (*fmt == '%') {
        out = sprintf_put(out, '%');
      }
      if (*fmt) {
        fmt++;
      }
    } else {
      out = sprintf_put(out, *fmt);
      fmt++;
    }
  }
  *out = 0;
  va_end(ap);
  return (int)(out - buf);
}

static int is_space(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }

int scanf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = 0;
  while (*fmt) {
    if (*fmt == '%') {
      fmt++;
      if (*fmt == 'd') {
        int *dst = va_arg(ap, int *);
        int c = getchar();
        while (is_space(c)) {
          c = getchar();
        }
        int neg = 0;
        if (c == '-') {
          neg = 1;
          c = getchar();
        }
        int val = 0;
        while (is_digit(c)) {
          val = val * 10 + (c - '0');
          c = getchar();
        }
        *dst = neg ? -val : val;
        n++;
      } else if (*fmt == 'c') {
        char *dst = va_arg(ap, char *);
        *dst = getchar();
        n++;
      } else if (*fmt == 's') {
        char *dst = va_arg(ap, char *);
        int c = getchar();
        while (is_space(c)) {
          c = getchar();
        }
        while (!is_space(c)) {
          *dst = c;
          dst++;
          c = getchar();
        }
        *dst = 0;
        n++;
      }
      if (*fmt) {
        fmt++;
      }
    } else {
      fmt++;
    }
  }
  va_end(ap);
  return n;
}

/* --- string.h --- */

unsigned int strlen(const char *s) {
  unsigned int n = 0;
  while (*s) {
    n++;
    s++;
  }
  return n;
}

char *strcpy(char *dst, const char *src) {
  char *ret = dst;
  while ((*dst = *src) != 0) {
    dst++;
    src++;
  }
  return ret;
}

char *strcat(char *dst, const char *src) {
  char *ret = dst;
  while (*dst) {
    dst++;
  }
  while ((*dst = *src) != 0) {
    dst++;
    src++;
  }
  return ret;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a - *b;
}

int strncmp(const char *a, const char *b, unsigned int n) {
  while (n && *a && *a == *b) {
    a++;
    b++;
    n--;
  }
  if (n == 0) {
    return 0;
  }
  return *a - *b;
}

void *memcpy(void *dst, const void *src, unsigned int n) {
  char *d = (char *)dst;
  const char *s = (const char *)src;
  while (n) {
    *d = *s;
    d++;
    s++;
    n--;
  }
  return dst;
}

void *memset(void *dst, int val, unsigned int n) {
  char *d = (char *)dst;
  while (n) {
    *d = (char)val;
    d++;
    n--;
  }
  return dst;
}

void *memmove(void *dst, const void *src, unsigned int n) {
  char *d = (char *)dst;
  const char *s = (const char *)src;
  if (d < s) {
    while (n) {
      *d = *s;
      d++;
      s++;
      n--;
    }
  } else {
    d += n;
    s += n;
    while (n) {
      d--;
      s--;
      *d = *s;
      n--;
    }
  }
  return dst;
}

int memcmp(const void *a, const void *b, unsigned int n) {
  const unsigned char *pa = (const unsigned char *)a;
  const unsigned char *pb = (const unsigned char *)b;
  while (n) {
    if (*pa != *pb) {
      return (int)*pa - (int)*pb;
    }
    pa++;
    pb++;
    n--;
  }
  return 0;
}

char *strncpy(char *dst, const char *src, unsigned int n) {
  unsigned int i = 0;
  while (i < n && src[i]) {
    dst[i] = src[i];
    i++;
  }
  while (i < n) {
    dst[i] = 0;
    i++;
  }
  return dst;
}

char *strncat(char *dst, const char *src, unsigned int n) {
  char *ret = dst;
  while (*dst) {
    dst++;
  }
  while (n && *src) {
    *dst = *src;
    dst++;
    src++;
    n--;
  }
  *dst = 0;
  return ret;
}

char *strdup(const char *s) {
  unsigned int n = strlen(s) + 1;
  char *p = malloc(n);
  if (!p) {
    return (void *)0;
  }
  memcpy(p, s, n);
  return p;
}

char *strchr(const char *s, int c) {
  while (*s) {
    if (*s == (char)c) {
      return (char *)s;
    }
    s++;
  }
  return (c == 0) ? (char *)s : (void *)0;
}

char *strrchr(const char *s, int c) {
  const char *last = (c == 0) ? s : (void *)0;
  while (*s) {
    if (*s == (char)c) {
      last = s;
    }
    s++;
  }
  return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
  if (!*needle) {
    return (char *)haystack;
  }
  while (*haystack) {
    const char *h = haystack;
    const char *n = needle;
    while (*h && *n && *h == *n) {
      h++;
      n++;
    }
    if (!*n) {
      return (char *)haystack;
    }
    haystack++;
  }
  return (void *)0;
}

/* --- ctype.h --- */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }

/* --- stdlib.h --- */

int atoi(const char *s) {
  while (is_space((unsigned char)*s)) {
    s++;
  }
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  int val = 0;
  while (is_digit((unsigned char)*s)) {
    val = val * 10 + (*s - '0');
    s++;
  }
  return neg ? -val : val;
}

/* No OS heap to draw from, so a fixed static arena stands in for one.
 * Bump-allocated only — free() is a real, callable no-op rather than
 * fake reclamation. See stdlib.h.
 */
#define HEAP_WORDS 1024
static char heap[HEAP_WORDS];
static unsigned int heap_used = 0;

void *malloc(unsigned int size) {
  if (heap_used + size > HEAP_WORDS) {
    return (void *)0;
  }
  void *p = &heap[heap_used];
  heap_used += size;
  return p;
}

void free(void *ptr) { (void)ptr; }

/* nmemb/size are both unsigned int (16-bit) per the standard signature
 * -- a plain 16-bit multiply, proven safe elsewhere (rand()'s LCG step
 * uses one too). Do NOT widen this to long: 32-bit multiply is known
 * broken on this target (see DEBUGGING_NOTES.md, "1000L * 17"). The
 * zero-fill reuses memset rather than a hand-rolled loop, both because
 * memset is already correct/tested and because a fresh loop here would
 * be indexing/dereferencing straight off a pointer parameter the same
 * way memset's own loop does (advance-then-dereference-bare) rather
 * than the broken bracket-index-a-pointer-variable shape.
 */
void *calloc(unsigned int nmemb, unsigned int size) {
  unsigned int total = nmemb * size;
  void *p = malloc(total);
  if (p) {
    memset(p, 0, total);
  }
  return p;
}

int abs(int n) { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }

/* Plain linear congruential generator -- period/quality don't matter
 * much for this target's likely uses, and 16-bit-native arithmetic
 * (both operands and the modulus fit in unsigned int's native word
 * width) avoids the 32-bit-multiply path entirely, which silently
 * computes the wrong answer on this backend (confirmed empirically:
 * 1000L times 17 came back 104, not 17000 -- a real, separate,
 * not-yet-root-caused backend bug; tracked as a known limitation, not
 * worked around here beyond simply not exercising it).
 */
static unsigned int rand_state = 1;

void srand(unsigned int seed) { rand_state = seed ? seed : 1; }

int rand(void) {
  rand_state = rand_state * 25173 + 13849;
  return (int)(rand_state & 0x7fff);
}

void exit(int status) {
  (void)status;
  asm volatile("HALT");
  for (;;) {
  }
}

static int digit_val(int c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'Z') {
    return c - 'A' + 10;
  }
  return -1;
}

/* val * base, base a small (<=36) runtime int: computed as base
 * additions of val rather than a real multiply -- see rand()'s
 * comment above on why 32-bit multiply isn't trustworthy on this
 * backend yet. Digit-at-a-time parsing only ever needs this once per
 * input character, so the O(base) cost here is not a real concern.
 */
static long long_mul_small(long val, int base) {
  long acc = 0;
  int i;
  for (i = 0; i < base; i++) {
    acc += val;
  }
  return acc;
}

long strtol(const char *s, char **endptr, int base) {
  while (isspace((unsigned char)*s)) {
    s++;
  }
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  /* Prefix peek via a copied-and-incremented pointer (assign, ++,
   * then dereference), never s[1] or a raw pointer-plus-one
   * dereference -- a real, confirmed backend bug: offsetting a
   * *local* pointer variable that holds a string-literal address and
   * then dereferencing it misreads (s[1] on "0x1a" read back '0',
   * s[0]'s own value, instead of 'x'), while the exact same pointer
   * advanced via ++ and then dereferenced bare reads correctly.
   * Matches this file's own established convention elsewhere
   * (strlen/strcpy/... never index a pointer variable either) --
   * apparently for the same reason.
   */
  if (*s == '0') {
    const char *peek = s;
    peek++;
    if ((base == 0 || base == 16) && (*peek == 'x' || *peek == 'X')) {
      s = peek;
      s++;
      base = 16;
    } else if (base == 0) {
      base = 8;
    }
  } else if (base == 0) {
    base = 10;
  }
  const char *start = s;
  long val = 0;
  int d;
  while ((d = digit_val((unsigned char)*s)) >= 0 && d < base) {
    val = long_mul_small(val, base) + d;
    s++;
  }
  if (endptr) {
    *endptr = (char *)(s == start ? start : s);
  }
  return neg ? -val : val;
}

unsigned long strtoul(const char *s, char **endptr, int base) {
  while (isspace((unsigned char)*s)) {
    s++;
  }
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  /* Prefix peek via a copied-and-incremented pointer (assign, ++,
   * then dereference), never s[1] or a raw pointer-plus-one
   * dereference -- a real, confirmed backend bug: offsetting a
   * *local* pointer variable that holds a string-literal address and
   * then dereferencing it misreads (s[1] on "0x1a" read back '0',
   * s[0]'s own value, instead of 'x'), while the exact same pointer
   * advanced via ++ and then dereferenced bare reads correctly.
   * Matches this file's own established convention elsewhere
   * (strlen/strcpy/... never index a pointer variable either) --
   * apparently for the same reason.
   */
  if (*s == '0') {
    const char *peek = s;
    peek++;
    if ((base == 0 || base == 16) && (*peek == 'x' || *peek == 'X')) {
      s = peek;
      s++;
      base = 16;
    } else if (base == 0) {
      base = 8;
    }
  } else if (base == 0) {
    base = 10;
  }
  const char *start = s;
  unsigned long val = 0;
  int d;
  while ((d = digit_val((unsigned char)*s)) >= 0 && d < base) {
    val = (unsigned long)long_mul_small((long)val, base) + (unsigned long)d;
    s++;
  }
  if (endptr) {
    *endptr = (char *)(s == start ? start : s);
  }
  return neg ? (unsigned long)(-(long)val) : val;
}

float atof(const char *s) {
  while (isspace((unsigned char)*s)) {
    s++;
  }
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  float val = 0.0f;
  while (isdigit((unsigned char)*s)) {
    val = val * 10.0f + (float)(*s - '0');
    s++;
  }
  if (*s == '.') {
    s++;
    float frac = 0.1f;
    while (isdigit((unsigned char)*s)) {
      val += (float)(*s - '0') * frac;
      frac *= 0.1f;
      s++;
    }
  }
  return neg ? -val : val;
}

/* --- soft float: IEEE-754 single precision (compiler-rt ABI) ---
 *
 * The backend's default libcall legalization (no f32/f64 register class
 * registered in EclipseISelLowering) already emits correct calls to
 * these exact symbol names for any `float` arithmetic/comparison/
 * conversion — see llvm/lib/Target/Eclipse/README.md's former "no
 * i32/float runtime library" limitation. This file is what was missing.
 *
 * This is IEEE-754 *single* precision only (`double` stays unimplemented
 * — it would need genuine 64-bit integer support this backend doesn't
 * have yet, per the same README). It's also deliberately *not* a fully
 * IEEE-compliant implementation: no NaN/Inf/subnormal input handling,
 * and rounding during mantissa alignment/normalization is plain
 * truncation rather than correctly-rounded (round-to-nearest-even) —
 * documented simplifications, not oversights, matching this project's
 * habit of recording gaps rather than silently leaving them. Overflow
 * saturates to the Inf bit pattern; underflow flushes to zero.
 *
 * Also deliberately avoids native 32-bit `*`/`/` on `long`/`unsigned
 * long` throughout: at the time this soft-float code was written, this
 * target had no i32 multiply/divide runtime at all (`__mulsi3`/
 * `__udivsi3` didn't exist), so using `*`/`/` here would just have
 * traded one missing runtime symbol for another. `__udivsi3`/
 * `__umodsi3`/`__divsi3`/`__modsi3` now exist below (see the "32-bit
 * integer division/remainder" section past print_uint32) — `__mulsi3`
 * still doesn't. This section's own multiply and divide stay as manual
 * bit-loops using only shifts, compares, and add/subtract regardless
 * (no need to churn already-working, already-verified code just to use
 * the now-available `/`), which *do* legalize natively (wide integer
 * comparison/add/sub decompose cleanly into 16-bit half operations;
 * multiply and divide don't, which is exactly why they need a libcall
 * in the first place).
 *
 * NB: this attached-FPU-device hardware (see fpu_out/fpu_in in
 * test_fps_add.c) uses its own, different floating-point format — none
 * of that applies here. This file's bit layout is plain IEEE-754,
 * independent of the device.
 */

typedef unsigned long u32;
typedef long i32;

#define SF_SIGN_MASK 0x80000000UL
#define SF_EXP_MASK 0x7F800000UL
#define SF_MANT_MASK 0x007FFFFFUL
#define SF_EXP_SHIFT 23
#define SF_EXP_BIAS 127
#define SF_HIDDEN_BIT (1UL << SF_EXP_SHIFT)

static u32 sf_bits(float f) {
  union {
    float f;
    u32 u;
  } x;
  x.f = f;
  return x.u;
}

static float sf_from_bits(u32 u) {
  union {
    float f;
    u32 u;
  } x;
  x.u = u;
  return x.f;
}

/* A 32-bit comparison used directly as a branch/select condition hits a
 * "Cannot select" crash in this backend — confirmed empirically (not
 * just the variable-shift issue this section's other comments describe;
 * this is a separate problem, still unresolved at the SelectionDAG
 * level even with ISD::BRCOND Custom-lowered and even at -O0). Routing
 * each 32-bit comparison through a real, non-inlined function-call
 * boundary sidesteps it: the caller then branches on an already-
 * materialized i16 boolean (a pattern already proven to work, same as
 * any other function returning int), instead of the compiler folding
 * the raw 32-bit compare directly into the branch/select it feeds.
 */
__attribute__((noinline)) static int u32_eq(u32 a, u32 b) { return a == b; }
__attribute__((noinline)) static int u32_ne(u32 a, u32 b) { return a != b; }
__attribute__((noinline)) static int u32_ge(u32 a, u32 b) { return a >= b; }
__attribute__((noinline)) static int u32_lt(u32 a, u32 b) { return a < b; }
__attribute__((noinline)) static int u32_gt(u32 a, u32 b) { return a > b; }
__attribute__((noinline)) static int u32_and_nz(u32 a, u32 b) { return (a & b) != 0; }
__attribute__((noinline)) static int i32_eq(long a, long b) { return a == b; }
__attribute__((noinline)) static int i32_lt(long a, long b) { return a < b; }

/* floor(num * 2^fracbits / den), via a plain restoring shift-subtract
 * long division loop — see this section's header comment for why this
 * can't just be `num / den`. `den` must be nonzero (callers only ever
 * pass a normalized, hidden-bit-set mantissa, which never is).
 */
/* Variable-*amount* shifts of a 32-bit value (`x >> n` where `n` isn't a
 * compile-time constant) lower to ISD::SRL_PARTS/SHL_PARTS, which this
 * backend has no pattern for — confirmed empirically ("Cannot select:
 * ... srl_parts ..."). Constant-amount shifts on wide values decompose
 * cleanly into plain 16-bit word ops and are fine (see e.g. sf_divbits'
 * `rem << 1` below); it's specifically the *runtime-computed* shift
 * amount that has no lowering. These do the shift one (compile-time-
 * constant) bit at a time instead, exactly the same workaround as
 * sf_divbits/the multiply loop use for the missing 32-bit multiply/
 * divide.
 */
static u32 sf_shr(u32 val, int amount) {
  while (amount > 0) {
    val >>= 1;
    amount--;
  }
  return val;
}

static u32 sf_shl(u32 val, int amount) {
  while (amount > 0) {
    val <<= 1;
    amount--;
  }
  return val;
}

static u32 sf_divbits(u32 num, u32 den, int fracbits) {
  u32 quotient = 0;
  u32 rem = 0;
  /* `num`'s bits are consumed MSB-first via a mask that itself only ever
   * shifts by the compile-time-constant 1 (see this section's header
   * comment on why: `num >> (i - fracbits)`, a *variable*-amount shift,
   * is exactly the pattern that doesn't lower on this target) — bit 23
   * is `num`'s own MSB, since callers only ever pass a 24-bit
   * (hidden-bit-inclusive) mantissa.
   */
  u32 mask = 1UL << 23;
  int total = 24 + fracbits;
  int i;
  for (i = total - 1; i >= 0; i--) {
    u32 bit;
    if (i >= fracbits && mask != 0) {
      bit = u32_and_nz(num, mask) ? 1UL : 0UL;
      mask >>= 1;
    } else {
      bit = 0;
    }
    rem = (rem << 1) | bit;
    quotient <<= 1;
    if (u32_ge(rem, den)) {
      rem -= den;
      quotient |= 1UL;
    }
  }
  return quotient;
}

/* sf_add split into several smaller functions rather than one big one:
 * confirmed empirically that a single function this size overflows the
 * ±127-word signed frame-relative displacement dgasm uses for local-
 * variable addressing ("Address out of range... should be -128 - 127"),
 * from spill-slot pressure (only AC0/AC1 are allocatable) rather than
 * from the ~24 words of named locals alone.
 *
 * Three things were tried here before landing on the current shape,
 * each confirmed broken (or confirmed *not* the bottleneck) by direct
 * testing, not assumption:
 *
 *   - Output-pointer parameters: every read/write through a pointer
 *     into an already-oversized frame goes through this backend's
 *     existing indirect-addressing workaround (the `_scratch`-based
 *     mechanism reorder_asm.py's "AddrSlots" comment describes for
 *     page-zero data), which made the frame bigger, not smaller.
 *
 *   - Pure value parameters/returns for every helper (no pointers, no
 *     cross-call globals): frame went *up*, not down (241 words, worse
 *     than one monolithic function's 209) — keeping several 32-bit
 *     values simultaneously live across many sequential calls (each
 *     call clobbers both allocatable registers, AC0/AC1, forcing
 *     spills) costs more than the reduction in named-local count saves.
 *
 *   - File-scope statics for cross-call state (this section's current
 *     shape): first attempt at this exposed what looked like a second,
 *     genuine backend bug — 32-bit (unsigned long) arithmetic
 *     corrupting its result whenever an operand round-tripped through a
 *     global. Fully root-caused since (two real, independent bugs, both
 *     now fixed):
 *
 *       1. EclipseAsmPrinter::emitGlobalVariable emitted a scalar
 *          initializer wider than one Eclipse word (e.g. `static u32 g
 *          = 0x40400000UL;`) as a *single* `var NAME = <decimal>` dgasm
 *          line. dgasm's `var` only ever reserves/deposits one 16-bit
 *          word, so the value silently truncated mod 2^16 at parse
 *          time — confirmed by tracing an isolated repro (two known-
 *          bit-pattern globals) all the way down to eclipseemu's actual
 *          deposited memory: both words of a 32-bit global came back
 *          0x0000. (0x40400000 truncated mod 2^16 happens to be exactly
 *          0, which is what made this look like a "high word right, low
 *          word wrong" *arithmetic* bug from the load side alone,
 *          rather than "the global was never really 32 bits wide in
 *          memory to begin with.") Fixed: multi-word scalar
 *          initializers now emit one `var` line per word,
 *          most-significant first, the same layout array initializers
 *          already used.
 *
 *       2. EclipseISelLowering.cpp's PerformDAGCombine halves a
 *          byte-granular runtime address offset to this word-addressed
 *          target's granularity, but only recognized a WRAPPER'd
 *          *global* address as the base of that ADD. The identical
 *          "access word N of a value that didn't fit in one register"
 *          pattern also happens for a *local* multi-word value (e.g.
 *          the second/low word of a 32-bit stack slot) whenever its
 *          FrameIndex base doesn't reduce to the bare-FrameIndex case
 *          EclipseISelDAGToDAG's custom LOAD/STORE handling absorbs
 *          directly — and unlike the global case, there's no
 *          LEAGA-offset-slot mechanism silently doing that division
 *          for it elsewhere. Left unhalved, it computed the wrong word
 *          address for that second word (base+2 instead of base+1).
 *          Fixed: the combine now also recognizes a bare FrameIndex
 *          base and halves a constant offset paired with one (the
 *          global-with-constant-offset exclusion — that offset is
 *          already word-granular by the time it would reach this
 *          combine — is unchanged).
 *
 *     With both fixed, file-scope statics for this function's cross-
 *     call state measure the smallest frame of the three approaches
 *     (see sf_add below) and are back in use.
 */
static u32 sf_pack(u32 mant) { return mant | SF_HIDDEN_BIT; }

static u32 sf_align_one(int keepexp, int otherexp, u32 m) {
  int diff = otherexp - keepexp;
  if (diff <= 0) {
    return m;
  }
  return (diff > 24) ? 0 : sf_shr(m, diff);
}

static int sf_align_rexp(int aexp, int bexp) {
  return (aexp >= bexp) ? aexp : bexp;
}

/* sf_addsub_result/sf_addsub_rsign used to be their own small functions
 * here, each independently re-deriving the same "same sign?"/"aM>=bM?"
 * conditions sf_add_finish (their only caller) needs anyway. Inlined
 * directly into sf_add_finish instead: two fewer call-slot page-zero
 * words (see this section's page-zero-budget comment) and two fewer
 * redundant u32_eq/u32_ge calls per sf_add — the difference that
 * finally fit sf_add's whole call graph inside the 256-word page-zero
 * budget alongside everything else already sharing it (printf's own
 * long-jump relaxation slots, in particular, confirmed via direct
 * before/after measurement to be the actual margin this bought).
 */

static float sf_normalize(u32 rsign, u32 result, int rexp) {
  if (u32_eq(result, 0)) {
    return sf_from_bits(0);
  }

  while (u32_ge(result, SF_HIDDEN_BIT << 1)) {
    result >>= 1;
    rexp++;
  }
  while (u32_lt(result, SF_HIDDEN_BIT) && rexp > 0) {
    result <<= 1;
    rexp--;
  }

  if (rexp <= 0) {
    return sf_from_bits(rsign);
  }
  if (rexp >= 255) {
    return sf_from_bits(rsign | SF_EXP_MASK);
  }
  return sf_from_bits(rsign | ((u32)rexp << SF_EXP_SHIFT) |
                       (result & SF_MANT_MASK));
}

/* Cross-call state for sf_add, below — file-scope statics rather than
 * locals/parameters specifically to keep sf_add's own frame small (see
 * this section's header comment for why: the other two approaches tried
 * both made the frame *bigger*). Not reentrancy-safe, but nothing here
 * ever calls sf_add (directly or indirectly) while another sf_add call
 * is still in progress — no recursion, single-threaded — so that's not
 * a real constraint in practice.
 */
/* Every field here costs at least one page-zero word (this backend
 * reaches *every* global through an indirect `_PTR` slot — see
 * EclipseInstrInfo.td's LEAGA comment — plus one more `_off1` word for
 * any field wider than one word), and page-zero is a shared, hard-
 * capped 256-word budget across the whole linked program (constant
 * pool entries, call slots, address slots, long-jump relaxation slots
 * — see reorder_asm.py's header comment). So beyond just "keep
 * sf_add's own frame small," the field *count and width* here matter
 * too — confirmed empirically: the first version of this (mirroring
 * sf_add's original locals one-for-one: separate `a`/`b`/`asign`/
 * `bsign`/`amant`/`bmant` fields alongside `aM`/`bM`) fixed the frame
 * overflow but then blew the page-zero budget instead ("Address out of
 * range... should be 0 - 255" on a long-jump relaxation slot elsewhere
 * in this same translation unit). Trimmed to the fields that actually
 * need to survive a function-call boundary, and narrowed each to the
 * smallest representation that still does: signs as a 0/1 `int` (the
 * mask itself is only ever needed transiently, reconstructed at each
 * use site) rather than the full 32-bit `SF_SIGN_MASK` value, "is this
 * operand zero" as a 0/1 `int` rather than keeping the whole mantissa
 * around, and the mantissa fields double as both the pre-alignment
 * (packed) and post-alignment (shifted) value rather than needing
 * separate fields for each stage.
 */
/* aneg/bneg/azero/bzero are four separate `int` fields rather than one
 * packed bitfield word — a packed-bitfield version was tried first and
 * measured *worse* on the page-zero budget despite three fewer page-
 * zero words of its own: each of the distinct bit-mask constants
 * (1/2/4/8) it needs costs its own constant-pool entry (also page-zero
 * — see reorder_asm.py's header comment), and the constant pool is
 * per-*function*, so sf_add_extract alone needed four new ones. That
 * cost more page-zero than the packing saved. Four separate 0/1 fields
 * only ever need the constant `1` (and `0`, needed everywhere already),
 * so they don't have this problem.
 */
static int sf_add_aneg, sf_add_bneg;
static int sf_add_azero, sf_add_bzero;
static int sf_add_aexp, sf_add_bexp;
static u32 sf_add_aM, sf_add_bM; /* packed mantissa (pre-align) */

static void sf_add_extract(float af, float bf) {
  u32 a = sf_bits(af), b = sf_bits(bf);
  u32 amant = a & SF_MANT_MASK, bmant = b & SF_MANT_MASK;
  sf_add_aexp = (int)((a & SF_EXP_MASK) >> SF_EXP_SHIFT);
  sf_add_bexp = (int)((b & SF_EXP_MASK) >> SF_EXP_SHIFT);
  sf_add_aneg = u32_and_nz(a, SF_SIGN_MASK) ? 1 : 0;
  sf_add_bneg = u32_and_nz(b, SF_SIGN_MASK) ? 1 : 0;
  sf_add_azero = (sf_add_aexp == 0 && u32_eq(amant, 0)) ? 1 : 0;
  sf_add_bzero = (sf_add_bexp == 0 && u32_eq(bmant, 0)) ? 1 : 0;
  sf_add_aM = sf_pack(amant);
  sf_add_bM = sf_pack(bmant);
}

/* Align back in its own void function (writing the aligned mantissas
 * back into sf_add_aM/sf_add_bM in place — sf_align_one for one of them
 * never depends on the other's value, only on aexp/bexp, so no temps
 * are needed to avoid clobbering an input before it's read) rather than
 * folded into sf_add_finish: with the addsub logic inlined below (see
 * that comment), sf_add_finish on its own was juggling `aM`/`bM`/`rexp`/
 * `asignv`/`bsignv`/`result`/`rsign` all at once and overflowed its
 * frame by a couple of words. Reading sf_add_aM/sf_add_bM directly
 * (rather than copying them into more locals first) once alignment has
 * already happened in a separate call removes two of those from
 * sf_add_finish's own live set, which was enough.
 */
static void sf_add_align(void) {
  sf_add_aM = sf_align_one(sf_add_aexp, sf_add_bexp, sf_add_aM);
  sf_add_bM = sf_align_one(sf_add_bexp, sf_add_aexp, sf_add_bM);
}

static float sf_add_finish(void) {
  int rexp = sf_align_rexp(sf_add_aexp, sf_add_bexp);
  u32 asignv = sf_add_aneg ? SF_SIGN_MASK : 0;
  u32 bsignv = sf_add_bneg ? SF_SIGN_MASK : 0;
  u32 result, rsign;

  if (u32_eq(asignv, bsignv)) {
    result = sf_add_aM + sf_add_bM;
    rsign = asignv;
  } else if (u32_ge(sf_add_aM, sf_add_bM)) {
    result = sf_add_aM - sf_add_bM;
    rsign = asignv;
  } else {
    result = sf_add_bM - sf_add_aM;
    rsign = bsignv;
  }

  return sf_normalize(rsign, result, rexp);
}

static float sf_add(float af, float bf) {
  sf_add_extract(af, bf);

  if (sf_add_azero) {
    return bf;
  }
  if (sf_add_bzero) {
    return af;
  }

  sf_add_align();
  return sf_add_finish();
}

float __addsf3(float a, float b) { return sf_add(a, b); }

float __subsf3(float a, float b) {
  return sf_add(a, sf_from_bits(sf_bits(b) ^ SF_SIGN_MASK));
}

float __negsf2(float a) { return sf_from_bits(sf_bits(a) ^ SF_SIGN_MASK); }

/* Same file-scope-static split as sf_add above (see its header comment
 * for the full rationale and the two failed alternatives) — __mulsf3's
 * original monolithic form hit the identical ±127-word frame overflow
 * (confirmed empirically, not assumed by analogy: "Address out of
 * range" up to -266 words). Extract/loop/finish, narrow 0/1 `int` flags
 * instead of full 32-bit masks, mantissa fields reused rather than
 * duplicated. The 24-iteration shift-and-add loop's own accumulator
 * variables (rhi/rlo/mlo/mhi/abit/i) stay ordinary locals *inside*
 * sf_mul_loop — they don't need to survive a function-call boundary,
 * only sf_mul_loop's own single invocation, so isolating them in their
 * own function (rather than needing them as statics too) keeps that
 * function's frame to just those six.
 */
static int sf_mul_rsign, sf_mul_zero;
static int sf_mul_aexp, sf_mul_bexp;
static u32 sf_mul_aM, sf_mul_bM;
static u32 sf_mul_rhi, sf_mul_rlo;

static void sf_mul_extract(float af, float bf) {
  u32 a = sf_bits(af), b = sf_bits(bf);
  u32 amant = a & SF_MANT_MASK, bmant = b & SF_MANT_MASK;
  sf_mul_aexp = (int)((a & SF_EXP_MASK) >> SF_EXP_SHIFT);
  sf_mul_bexp = (int)((b & SF_EXP_MASK) >> SF_EXP_SHIFT);
  sf_mul_rsign = u32_and_nz(a ^ b, SF_SIGN_MASK) ? 1 : 0;
  sf_mul_zero = ((sf_mul_aexp == 0 && u32_eq(amant, 0)) ||
                 (sf_mul_bexp == 0 && u32_eq(bmant, 0)))
                    ? 1
                    : 0;
  sf_mul_aM = sf_pack(amant);
  sf_mul_bM = sf_pack(bmant);
}

/* Plain native comparisons/bitwise-AND here (not the u32_* call-boundary
 * wrappers used elsewhere in this section) — those wrappers exist to
 * work around what turned out to be a *global-initializer-truncation*
 * bug (see EclipseAsmPrinter.cpp's emitGlobalVariable comment), not a
 * general "32-bit ops on locals are unsafe" one, and every value this
 * loop touches is a plain local, never a global. A branchless rewrite
 * (compute a zero-or-real addend, add unconditionally, no `if`) was
 * also tried and made the frame *worse*, not better — reverted. What
 * actually got this under the ±127-word limit: fewer named temporaries
 * per iteration (folding `old`/`carry`/`carrybit` into the expressions
 * that use them once, rather than naming each), on top of dropping the
 * wrapper calls above.
 */
static void sf_mul_loop(void) {
  u32 rhi = 0, rlo = 0;
  u32 mlo = sf_mul_bM, mhi = 0;
  u32 abit = 1UL;
  int i;
  for (i = 0; i < 24; i++) {
    if (sf_mul_aM & abit) {
      rlo += mlo;
      rhi += mhi + ((rlo < mlo) ? 1UL : 0UL);
    }
    mhi = (mhi << 1) | ((mlo & 0x80000000UL) ? 1UL : 0UL);
    mlo <<= 1;
    abit <<= 1;
  }
  sf_mul_rhi = rhi;
  sf_mul_rlo = rlo;
}

static float sf_mul_finish(void) {
  u32 rsignv = sf_mul_rsign ? SF_SIGN_MASK : 0;
  /* Product of two 24-bit (hidden-bit-inclusive) mantissas lies in
   * [2^46, 2^48), so its MSB is product-bit 46 or 47 — exactly two
   * cases, distinguished by rhi's bit 15 (product-bit 47). */
  int rexp = sf_mul_aexp + sf_mul_bexp - SF_EXP_BIAS;
  u32 rman24;
  if (u32_and_nz(sf_mul_rhi, 0x8000UL)) {
    rman24 = (sf_mul_rhi << 8) | (sf_mul_rlo >> 24);
    rexp += 1;
  } else {
    rman24 = ((sf_mul_rhi & 0x7FFFUL) << 9) | (sf_mul_rlo >> 23);
  }

  if (rexp <= 0) {
    return sf_from_bits(rsignv);
  }
  if (rexp >= 255) {
    return sf_from_bits(rsignv | SF_EXP_MASK);
  }
  return sf_from_bits(rsignv | ((u32)rexp << SF_EXP_SHIFT) |
                       (rman24 & SF_MANT_MASK));
}

float __mulsf3(float af, float bf) {
  sf_mul_extract(af, bf);
  if (sf_mul_zero) {
    return sf_from_bits(sf_mul_rsign ? SF_SIGN_MASK : 0);
  }
  sf_mul_loop();
  return sf_mul_finish();
}

/* Same split again for __divsf3 — same empirically-confirmed frame
 * overflow, same fix.
 */
static int sf_div_rsign, sf_div_zero_divisor, sf_div_zero_result;
static int sf_div_aexp, sf_div_bexp;
static u32 sf_div_aM, sf_div_bM, sf_div_raw;

static void sf_div_extract(float af, float bf) {
  u32 a = sf_bits(af), b = sf_bits(bf);
  u32 amant = a & SF_MANT_MASK, bmant = b & SF_MANT_MASK;
  sf_div_aexp = (int)((a & SF_EXP_MASK) >> SF_EXP_SHIFT);
  sf_div_bexp = (int)((b & SF_EXP_MASK) >> SF_EXP_SHIFT);
  sf_div_rsign = u32_and_nz(a ^ b, SF_SIGN_MASK) ? 1 : 0;
  sf_div_zero_divisor = (sf_div_bexp == 0 && u32_eq(bmant, 0)) ? 1 : 0;
  sf_div_zero_result = (sf_div_aexp == 0 && u32_eq(amant, 0)) ? 1 : 0;
  sf_div_aM = sf_pack(amant);
  sf_div_bM = sf_pack(bmant);
}

static void sf_div_compute(void) {
  /* floor(aM * 2^24 / bM); aM,bM in [2^23,2^24) so aM/bM in (0.5,2),
   * putting this quotient in (2^23, 2^25). */
  sf_div_raw = sf_divbits(sf_div_aM, sf_div_bM, 24);
}

static float sf_div_finish(void) {
  u32 rsignv = sf_div_rsign ? SF_SIGN_MASK : 0;
  int rexp = sf_div_aexp - sf_div_bexp + SF_EXP_BIAS;
  u32 rman24;
  if (u32_and_nz(sf_div_raw, 1UL << 24)) {
    rman24 = sf_div_raw >> 1;
  } else {
    rman24 = sf_div_raw;
    rexp -= 1;
  }

  if (rexp <= 0) {
    return sf_from_bits(rsignv);
  }
  if (rexp >= 255) {
    return sf_from_bits(rsignv | SF_EXP_MASK);
  }
  return sf_from_bits(rsignv | ((u32)rexp << SF_EXP_SHIFT) |
                       (rman24 & SF_MANT_MASK));
}

float __divsf3(float af, float bf) {
  sf_div_extract(af, bf);
  if (sf_div_zero_divisor) {
    /* Division by zero: no real Inf/NaN support (see header comment),
     * but return the correctly-signed Inf bit pattern anyway rather
     * than looping or returning garbage. */
    return sf_from_bits((sf_div_rsign ? SF_SIGN_MASK : 0) | SF_EXP_MASK);
  }
  if (sf_div_zero_result) {
    return sf_from_bits(sf_div_rsign ? SF_SIGN_MASK : 0);
  }
  sf_div_compute();
  return sf_div_finish();
}

/* -1/0/1 three-way compare. All six libgcc-style comparison libcalls
 * share this: __eqsf2/__nesf2 test the result against zero, __ltsf2/
 * __lesf2/__gtsf2/__gesf2 use the signed result directly — the same
 * convention real compiler-rt uses, which is why one function can back
 * all six. No unordered/NaN handling (see header comment).
 */
static int sf_cmp(float af, float bf) {
  u32 a = sf_bits(af), b = sf_bits(bf);
  u32 asign = a & SF_SIGN_MASK, bsign = b & SF_SIGN_MASK;
  u32 amag = a & 0x7FFFFFFFUL, bmag = b & 0x7FFFFFFFUL;

  if (u32_eq(amag, 0) && u32_eq(bmag, 0)) {
    return 0; /* +0 == -0 */
  }
  if (u32_ne(asign, bsign)) {
    return u32_ne(asign, 0) ? -1 : 1;
  }
  /* Same sign: for normal IEEE-754 bit patterns, comparing the raw
   * magnitude bits gives the same order as comparing the values they
   * represent (exponent occupies the high bits, same as a value
   * comparison would weight it) — then flip for negative operands. */
  int magcmp = u32_lt(amag, bmag) ? -1 : u32_gt(amag, bmag) ? 1 : 0;
  return u32_ne(asign, 0) ? -magcmp : magcmp;
}

int __eqsf2(float a, float b) { return sf_cmp(a, b); }
int __nesf2(float a, float b) { return sf_cmp(a, b); }
int __ltsf2(float a, float b) { return sf_cmp(a, b); }
int __lesf2(float a, float b) { return sf_cmp(a, b); }
int __gtsf2(float a, float b) { return sf_cmp(a, b); }
int __gesf2(float a, float b) { return sf_cmp(a, b); }

/* Ordered-comparison softening pairs the primary predicate with an
 * unordered check (to filter out NaN operands) — always 0 (never
 * unordered) since this implementation doesn't represent NaN specially
 * (see this section's header comment).
 */
int __unordsf2(float a, float b) {
  (void)a;
  (void)b;
  return 0;
}

float __floatsisf(long i) {
  if (i32_eq(i, 0)) {
    return sf_from_bits(0);
  }
  u32 sign = 0;
  u32 mag;
  if (i32_lt(i, 0)) {
    sign = SF_SIGN_MASK;
    mag = (u32)(-(i + 1)) + 1UL; /* negate without overflowing at LONG_MIN */
  } else {
    mag = (u32)i;
  }

  int pos = 31;
  u32 posmask = 1UL << 31; /* 31 is compile-time constant, so this init
                             * doesn't need a variable-amount shift —
                             * see this section's header comment. */
  while (!u32_and_nz(mag, posmask)) {
    posmask >>= 1;
    pos--;
  }
  int rexp = pos + SF_EXP_BIAS;
  u32 rman24 = (pos >= SF_EXP_SHIFT) ? sf_shr(mag, pos - SF_EXP_SHIFT)
                                      : sf_shl(mag, SF_EXP_SHIFT - pos);
  return sf_from_bits(sign | ((u32)rexp << SF_EXP_SHIFT) |
                       (rman24 & SF_MANT_MASK));
}

float __floatunsisf(u32 mag) {
  if (u32_eq(mag, 0)) {
    return sf_from_bits(0);
  }
  int pos = 31;
  u32 posmask = 1UL << 31; /* 31 is compile-time constant, so this init
                             * doesn't need a variable-amount shift —
                             * see this section's header comment. */
  while (!u32_and_nz(mag, posmask)) {
    posmask >>= 1;
    pos--;
  }
  int rexp = pos + SF_EXP_BIAS;
  u32 rman24 = (pos >= SF_EXP_SHIFT) ? sf_shr(mag, pos - SF_EXP_SHIFT)
                                      : sf_shl(mag, SF_EXP_SHIFT - pos);
  return sf_from_bits(((u32)rexp << SF_EXP_SHIFT) | (rman24 & SF_MANT_MASK));
}

/* Truncates toward zero, per the C conversion's own rules. */
long __fixsfsi(float f) {
  u32 a = sf_bits(f);
  u32 sign = a & SF_SIGN_MASK;
  int aexp = (int)((a & SF_EXP_MASK) >> SF_EXP_SHIFT);
  u32 amant = a & SF_MANT_MASK;
  if (aexp == 0) {
    return 0;
  }
  u32 aM = amant | SF_HIDDEN_BIT;
  int shift = aexp - SF_EXP_BIAS - SF_EXP_SHIFT;
  u32 mag;
  if (shift >= 8) {
    mag = u32_ne(sign, 0) ? 0x80000000UL : 0x7FFFFFFFUL; /* saturate on overflow */
  } else if (shift >= 0) {
    mag = sf_shl(aM, shift);
  } else if (shift > -24) {
    mag = sf_shr(aM, -shift);
  } else {
    mag = 0;
  }
  return u32_ne(sign, 0) ? -(long)mag : (long)mag;
}

u32 __fixunssfsi(float f) {
  u32 a = sf_bits(f);
  if (u32_and_nz(a, SF_SIGN_MASK)) {
    return 0; /* negative -> unsigned: no valid result, clamp to 0 */
  }
  int aexp = (int)((a & SF_EXP_MASK) >> SF_EXP_SHIFT);
  u32 amant = a & SF_MANT_MASK;
  if (aexp == 0) {
    return 0;
  }
  u32 aM = amant | SF_HIDDEN_BIT;
  int shift = aexp - SF_EXP_BIAS - SF_EXP_SHIFT;
  if (shift >= 8) {
    return 0xFFFFFFFFUL; /* saturate */
  }
  if (shift >= 0) {
    return sf_shl(aM, shift);
  }
  if (shift > -24) {
    return sf_shr(aM, -shift);
  }
  return 0;
}

/* --- print_float: decimal formatting of a float ---
 *
 * Deliberately NOT wired into printf()'s '%f' — printf() is called by
 * essentially every program, and internalize/globaldce (see eclipse-cc's
 * build_and_assemble) decides what's reachable from a plain, static IR
 * call graph. A `printf` that called print_float directly would make
 * print_float — and everything IT calls (__fixsfsi, __mulsf3, sf_add,
 * ...: almost this entire file) — permanently reachable from *any*
 * program that calls printf at all, float or not, since reachability
 * can't see that a given call site's format string never contains "%f".
 * Confirmed empirically: even a bare `printf("%d\n", 42)` failed to
 * assemble ("Address out of range" on dozens of soft-float symbols)
 * once %f lived inside printf's switch — the whole shared 256-word
 * page-zero budget was gone before `main` did anything.
 *
 * The float-arithmetic RTLIB calls (__addsf3 etc.) don't have this
 * problem because `llc` inserts *those* during instruction selection,
 * after internalize/globaldce has already run — invisible to it either
 * way, which is exactly why eclipse-cc's iterative "Undefined symbol"
 * retry loop exists. print_float is an ordinary, explicit C call with
 * no such trick available, so it has to stay opt-in: call print_float(f)
 * directly wherever a program actually wants a float printed, instead
 * of printf("%f", f). Non-float programs (still the common case) pay
 * nothing for its existence.
 */

/* Restoring binary long division of a full 32-bit `val` by the constant
 * 10, in the same style as sf_divbits/sf_shr/sf_shl above: MSB-first bit
 * extraction via a mask that itself only ever shifts by the compile-time
 * constant 1 (`mask >>= 1`), never `val >> i` for a runtime-variable `i`
 * — that's the ISD::SRL_PARTS "Cannot select" pattern this whole file's
 * bit-by-bit-loop convention exists to avoid. This is print_float's only
 * reason for existing: print_uint/print_octal above take a 16-bit
 * `unsigned int`, but a float's integer part can easily exceed 16 bits
 * (any value >= 32768.0f), and this backend has no native 32-bit `/`/`%`
 * (no __udivsi3) to fall back on for an ordinary `val / 10`.
 */
/* The remainder comes back through a static, not a `u32 *rem_out` output
 * parameter — confirmed empirically (via an isolated test calling this
 * directly, bypassing print_float entirely) that storing a 32-bit value
 * through a pointer PARAMETER silently discards it on this backend: the
 * quotient (an ordinary 2-word return) came back correct every time,
 * but `*rem_out = rem` never actually reached the caller's variable —
 * always read back 0. A genuine, previously-unexercised backend bug
 * (nothing else in this file writes a wide value through a pointer
 * *parameter* — sf_add etc.'s statics were chosen for frame size, not
 * because of this), not something worth chasing further here given the
 * static-communication pattern already used throughout this file
 * (pf_frac_bits, sf_add_aM, ...) sidesteps it entirely.
 */
static u32 u32_div10_rem;

u32 u32_div10(u32 val) {
  u32 quotient = 0;
  u32 rem = 0;
  u32 mask = 0x80000000UL;
  int i;
  for (i = 31; i >= 0; i--) {
    u32 bit = u32_and_nz(val, mask) ? 1UL : 0UL;
    mask >>= 1;
    rem = (rem << 1) | bit;
    quotient <<= 1;
    if (u32_ge(rem, 10UL)) {
      rem -= 10UL;
      quotient |= 1UL;
    }
  }
  u32_div10_rem = rem;
  return quotient;
}

static int print_uint32(u32 val) {
  int n = 0;
  u32 q = u32_div10(val);
  /* Captured into a local *before* recursing: the recursive call below
   * calls u32_div10 again, which overwrites u32_div10_rem with its own
   * result — reading the static only after that call returned would
   * silently pick up the wrong (innermost) remainder. */
  u32 rem = u32_div10_rem;
  if (u32_ge(val, 10UL)) {
    n += print_uint32(q);
  }
  putchar('0' + (int)rem);
  return n + 1;
}

/* --- hardware-float (`--hwfloat`) IEEE<->hex-float bridge ---------------
 *
 * The optional `+hwfloat` SubtargetFeature (off by default -- see the
 * LLVM backend's Eclipse.td FeatureHWFloat) replaces __addsf3/__subsf3
 * with __addsf3_hw/__subsf3_hw: a hand-written assembly file
 * (rt/eclipse_hwfloat.s, NOT compiled from C -- this backend has no
 * SelectionDAG support for FLDS/FAS/FSTS and Clang has no inline-asm
 * for this target) that runs the real Eclipse S/140 FPU instead of this
 * section's own software arithmetic above. That hardware FPU's "short"
 * (single-precision) format is IBM System/360-style hex-float --
 * sign + 7-bit excess-64 exponent over powers of *16* + 24-bit
 * hex-digit-normalized mantissa -- a genuinely different bit layout
 * from the IEEE-754 this file's own `float` uses everywhere else (the
 * ABI, printf/print_float, struct layout, ...). See DEBUGGING_NOTES.md
 * for the reverse-engineered format itself and the evidence behind it.
 *
 * These two functions convert between the two formats; eclipse_hwfloat.s
 * calls them (via an ordinary EJSR, same calling convention as any other
 * function here) at the entry/exit of __addsf3_hw/__subsf3_hw, so
 * nothing outside those two functions' own bodies ever needs to know the
 * hardware format exists -- every other piece of this target's float
 * support (this section's own software arithmetic included) keeps using
 * IEEE-754 exclusively, unconditionally, same as before this feature
 * existed. Only ever reachable (hence only ever protected from
 * globaldce, and only ever costing page-zero budget) on a program built
 * with --hwfloat -- see eclipse-cc's own comment on why that protection
 * is automatic and needs no special-casing here.
 *
 * Ported from a standalone Python implementation cross-checked
 * bit-for-bit against real `eclipseemu` FAS/FSS results (see
 * DEBUGGING_NOTES.md for the verification) -- not derived fresh here.
 * Same "not fully IEEE-compliant" simplifications as the rest of this
 * section: no subnormal input handling, overflow saturates to the
 * largest representable magnitude, underflow flushes to zero. The
 * IEEE->hex direction rounds to nearest (not truncating) at the 24-bit
 * hex mantissa boundary -- but hex float's 4-bit-at-a-time exponent
 * granularity still means up to 3 bits of a value's true precision
 * can't be represented exactly ("wobble"), an inherent property of this
 * hardware's own number format, not a bug in this conversion. Uses only
 * sf_shl/sf_shr (never a raw runtime-variable-amount `<<`/`>>`) and
 * u32_eq/u32_lt/u32_ge for every 32-bit comparison, for the exact same
 * SRL_PARTS/SHL_PARTS and 32-bit-branch-condition reasons documented on
 * those helpers above.
 */
/* Non-static (external linkage), unlike this file's other internal
 * helpers: called by NAME from eclipse_hwfloat.s's hand-written
 * assembly, which -- like __addsf3_hw/__subsf3_hw's own libcall names --
 * needs the exact, unmangled symbol this backend emits for an ordinary
 * external C function (matching sf_bits/__addsf3/etc.'s own reasoning
 * above). Never referenced from any LLVM IR call site (only from raw
 * assembly text added after `llc` runs -- see eclipse_hwfloat.s), so
 * globaldce always considers it unreachable unless a --hwfloat build
 * explicitly protects it -- eclipse-cc's existing "protect exactly the
 * undefined symbols dgasm reports" retry loop already does this
 * automatically, the same generic mechanism that already covers
 * __addsf3_hw/__subsf3_hw themselves and every other runtime symbol
 * here; no special-casing needed for these two specifically. */
u32 ieee754_to_hexfloat32(u32 bits) {
  u32 sign, mant23, sig24, hexmant24;
  int exp8, e, s;

  if (u32_eq(bits, 0) || u32_eq(bits, SF_SIGN_MASK)) {
    return bits & SF_SIGN_MASK; /* +-0.0 -> hex-float zero, sign kept */
  }
  sign = bits & SF_SIGN_MASK;
  exp8 = (int)((bits & SF_EXP_MASK) >> SF_EXP_SHIFT);
  mant23 = bits & SF_MANT_MASK;
  if (exp8 == 0) {
    return sign; /* subnormal input -- flush to zero */
  }
  if (exp8 == 255) {
    return sign | 0x7F000000UL | 0x00FFFFFFUL; /* inf/nan -- saturate */
  }

  /* value = sig24 * 2^(exp8-127-23) = 0.1(mant23 bits) * 2^total_shift */
  sig24 = SF_HIDDEN_BIT | mant23;
  {
    int total_shift = exp8 - SF_EXP_BIAS + 1;
    /* e = ceil(total_shift / 4), s = 4*e - total_shift (s in 0..3) --
     * C's `/` truncates toward zero, which only equals ceil-division
     * directly for a non-negative numerator, hence the two branches. */
    e = total_shift >= 0 ? (total_shift + 3) / 4 : -((-total_shift) / 4);
    s = 4 * e - total_shift;
  }

  if (s > 0) {
    hexmant24 = sf_shr(sig24 + sf_shl(1UL, s - 1), s); /* round to nearest */
  } else {
    hexmant24 = sig24;
  }
  if (u32_ge(hexmant24, 1UL << 24)) {
    /* rounding carried out of the top -- renormalize by one hex digit */
    hexmant24 = sf_shr(hexmant24, 4);
    e++;
  }
  hexmant24 &= 0x00FFFFFFUL;

  {
    int exp_biased = e + 64;
    if (exp_biased < 0) {
      return sign; /* underflow */
    }
    if (exp_biased > 127) {
      return sign | 0x7F000000UL | 0x00FFFFFFUL; /* overflow */
    }
    return sign | ((u32)(exp_biased & 0x7F) << 24) | hexmant24;
  }
}

/* Non-static -- see ieee754_to_hexfloat32's own comment just above. */
u32 hexfloat32_to_ieee754(u32 bits) {
  u32 sign, hexmant24;
  int exp_biased, e, shift, exp2, exp8;

  if (u32_eq(bits, 0) || u32_eq(bits, SF_SIGN_MASK)) {
    return bits & SF_SIGN_MASK;
  }
  sign = bits & SF_SIGN_MASK;
  exp_biased = (int)((bits >> 24) & 0x7FUL);
  hexmant24 = bits & 0x00FFFFFFUL;
  if (u32_eq(hexmant24, 0)) {
    return sign;
  }
  e = exp_biased - 64;

  /* Normalize so bit 23 (SF_HIDDEN_BIT) is set -- mirrors sf_normalize's
   * own single-bit-at-a-time loop above; a hex-float mantissa can have
   * up to 3 leading zero *bits* (it's only normalized to a nonzero hex
   * *digit*, a coarser 4-bit granularity) that IEEE's single-bit
   * normalization needs squeezed out. */
  shift = 0;
  while (u32_lt(hexmant24, SF_HIDDEN_BIT)) {
    hexmant24 <<= 1;
    shift++;
  }

  /* value = 0.hexmant24(24 bits) * 16^e = hexmant24 * 2^(4e-24)
   *       = (1.mant23) * 2^(4e-1-shift), reading hexmant24 (now
   *       normalized, bit23 set) as an IEEE-style 1.mant23 significand. */
  exp2 = 4 * e - 1 - shift;
  exp8 = exp2 + SF_EXP_BIAS;
  if (exp8 <= 0) {
    return sign; /* underflow */
  }
  if (exp8 >= 255) {
    return sign | SF_EXP_MASK; /* overflow -> Inf */
  }
  return sign | ((u32)exp8 << SF_EXP_SHIFT) | (hexmant24 & SF_MANT_MASK);
}

/* --- Hardware-FPU-path range guards for int<->float conversion, used
 * from rt/eclipse_hwfloat.s's hand-written __floatsisf_hw/
 * __floatunsisf_hw/__fixsfsi_hw/__fixunssfsi_hw (see that file's own
 * header comment for the full design). Ordinary compiled C, not hand-
 * written assembly, on purpose: this is plain 32-bit integer/exponent
 * range logic that the compiler already handles correctly everywhere
 * else in this file, so there's no reason to hand-roll it in assembly
 * and take on that risk for no benefit -- hand-written assembly here
 * is reserved for what genuinely has no other path (driving FLAS/FFAS/
 * FCMP/FSS themselves).
 *
 * __hwf_fits_pos16 backs BOTH __floatsisf_hw and __floatunsisf_hw.
 * FLAS (int->float) was confirmed EMPIRICALLY reliable only for a
 * value in [0,32767] -- NOT the full 16-bit signed range one might
 * expect: every negative value probed (-1, -2, -5, -100, -32767,
 * -32768) came back as a wildly wrong huge-magnitude float (e.g. the
 * bit pattern for int -5 decoded to roughly -16744453.0, not -5.0),
 * while every nonnegative value probed (0, 1, 2, 5, 100, 32767) came
 * back exactly correct. So the *same* [0,32767] guard is the correct,
 * safe range for both the signed and unsigned direction -- there is no
 * wider safe range for the signed case the way there might naively be
 * expected, and checking the raw bit pattern against 32768 this way
 * correctly rejects a negative `long` (whose bit pattern reinterpreted
 * as unsigned is far above 32768) exactly as it should.
 */
int __hwf_fits_pos16(unsigned long v) { return u32_lt(v, 32768UL); }

/* __hwf_fits_i16f/__hwf_fits_u16f back __fixsfsi_hw/__fixunssfsi_hw
 * (float->int, via FFAS). FFAS turned out to behave the OPPOSITE way
 * from FLAS: confirmed empirically reliable across the full signed
 * 16-bit range INCLUDING negative values and the -32768 boundary
 * (-1.0, -100.0, -16384.0, -32768.0 all truncated exactly correctly),
 * but confirmed unreliable once the true magnitude reaches 32768 or
 * beyond (silently returns 0 instead of the true low-order bits, the
 * same "executes without error but silently wrong" failure class as
 * FMS/FDS, just narrower) -- so these only need to bound magnitude via
 * the IEEE-754 exponent field, not sign. `SF_EXP_BIAS + 14` is exactly
 * the largest biased exponent guaranteeing |f| < 32768 (2^15): a
 * *biased* exponent of `SF_EXP_BIAS + 15` (unbiased 15) already means
 * |f| is in [32768,65536) -- unsafe -- so this threshold is exact, not
 * a heuristic margin.
 */
int __hwf_fits_i16f(float f) {
  u32 a = sf_bits(f);
  int aexp = (int)((a & SF_EXP_MASK) >> SF_EXP_SHIFT);
  return aexp <= (SF_EXP_BIAS + 14);
}

int __hwf_fits_u16f(float f) {
  u32 a = sf_bits(f);
  if (u32_and_nz(a, SF_SIGN_MASK)) {
    return 0; /* negative -> unsigned conversion is never safe */
  }
  int aexp = (int)((a & SF_EXP_MASK) >> SF_EXP_SHIFT);
  return aexp <= (SF_EXP_BIAS + 14);
}

/* --- 32-bit integer division/remainder (RTLIB::UDIV_I32/SDIV_I32/
 * UREM_I32/SREM_I32, i.e. __udivsi3/__divsi3/__umodsi3/__modsi3) ---
 *
 * MVT::i32 has no register class in EclipseISelLowering.cpp (only i16
 * does), and only i16 SDIV/SREM get Custom lowering there (the
 * hardware DIV instruction, UDIVrr/UREMrr, is native only for 16-bit
 * operands — see that file's LowerSDIVREM). Any 32-bit `/` or `%`
 * (this target's `long`/`unsigned long`) therefore falls through the
 * type legalizer's default path straight to a libcall, exactly like
 * every `float` op does — see this file's soft-float section header
 * comment above for the general mechanism. These four functions are
 * what was missing (previously: `llc` hard-crashed with "unsupported
 * library call operation" the moment any program divided a `long`).
 *
 * Same restoring shift-subtract long division as u32_div10 above,
 * generalized to a runtime-variable divisor instead of the
 * compile-time constant 10 — see u32_div10's own comment for why it's
 * built this way: each shift is by the compile-time-constant 1,
 * looped at runtime, never a variable-amount shift (ISD::SRL_PARTS/
 * SHL_PARTS have no pattern on this backend), and every 32-bit
 * comparison is routed through a noinline helper (a raw 32-bit
 * compare feeding a branch/select directly hits a separate "Cannot
 * select" crash — see the u32_eq/u32_ge/... comment above). The
 * remainder comes back through a static rather than an output
 * parameter for the same reason u32_div10_rem does (see that
 * variable's comment): a 32-bit value written through a pointer
 * *parameter* is silently discarded on this backend.
 *
 * `den == 0` is undefined behavior in C, same as any other target's
 * __udivsi3/__umodsi3 — not special-cased here.
 */
static u32 u32_divmod_rem;

static u32 u32_divmod(u32 num, u32 den) {
  u32 quotient = 0;
  u32 rem = 0;
  u32 mask = 0x80000000UL;
  int i;
  for (i = 31; i >= 0; i--) {
    u32 bit = u32_and_nz(num, mask) ? 1UL : 0UL;
    mask >>= 1;
    rem = (rem << 1) | bit;
    quotient <<= 1;
    if (u32_ge(rem, den)) {
      rem -= den;
      quotient |= 1UL;
    }
  }
  u32_divmod_rem = rem;
  return quotient;
}

u32 __udivsi3(u32 num, u32 den) { return u32_divmod(num, den); }

u32 __umodsi3(u32 num, u32 den) {
  u32_divmod(num, den);
  return u32_divmod_rem;
}

/* Signed division/remainder built on the unsigned primitive above,
 * same sign-handling convention as LowerSDIVREM in
 * EclipseISelLowering.cpp (this file's C-level equivalent, for the
 * 32-bit case that backend function can't itself lower): divide/mod
 * the absolute values, then reapply the sign — quotient is negative
 * iff exactly one operand was negative, remainder takes the
 * dividend's sign (C's truncating-division rule). `-(long)LONG_MIN`
 * overflows a plain negate, so the magnitude is taken the same
 * "negate without overflowing" way __floatsisf above does it.
 */
static u32 i32_mag(long x) {
  return i32_lt(x, 0) ? (u32)(-(x + 1)) + 1UL : (u32)x;
}

long __divsi3(long a, long b) {
  u32 uq = u32_divmod(i32_mag(a), i32_mag(b));
  int neg = i32_lt(a, 0) != i32_lt(b, 0);
  return neg ? -(long)uq : (long)uq;
}

long __modsi3(long a, long b) {
  u32_divmod(i32_mag(a), i32_mag(b));
  u32 ur = u32_divmod_rem;
  return i32_lt(a, 0) ? -(long)ur : (long)ur;
}

/* print_float split into two functions, communicating through file-scope
 * statics, for the exact same reason sf_add/__mulsf3/__divsf3 all had to
 * be split this same way (see those functions' own header comments):
 * confirmed empirically that print_float as a single function overflows
 * the ±127-word signed frame-relative displacement dgasm uses for
 * local-variable addressing ("Address out of range... should be -128 -
 * 127"), from spill-slot pressure (only AC0/AC1 are allocatable) rather
 * than the raw count of locals.
 */
static u32 pf_frac_bits;
static int pf_fbits_n;

/* Prints the sign and integer part, and computes pf_frac_bits/
 * pf_fbits_n for print_float_frac below to consume — a fixed-point
 * binary fraction (pf_frac_bits holds the low pf_fbits_n bits of the
 * mantissa, i.e. the value's fractional part is pf_frac_bits /
 * 2^pf_fbits_n) extracted directly from the mantissa, deliberately NOT
 * via `f - (float)(long)f` (needs __subsf3, i.e. all of sf_add) — see
 * print_float's own comment below for why that matters here.
 */
static void print_float_extract(float f) {
  u32 bits = sf_bits(f);
  if (u32_and_nz(bits, SF_SIGN_MASK)) {
    putchar('-');
  }
  long ip = __fixsfsi(f);
  u32 uip = (ip < 0) ? (u32)(-ip) : (u32)ip;
  print_uint32(uip);
  putchar('.');

  u32 mbits = bits & ~SF_SIGN_MASK;
  int aexp = (int)((mbits & SF_EXP_MASK) >> SF_EXP_SHIFT);
  pf_frac_bits = 0;
  pf_fbits_n = 0;
  if (aexp != 0) {
    u32 aM = (mbits & SF_MANT_MASK) | SF_HIDDEN_BIT;
    /* shift >= 0 means the value is a pure integer (all of aM's bits
     * are at or above the binary point) — pf_fbits_n/pf_frac_bits are
     * left at 0 for that case, and for the aexp==0 (zero/denormal) case
     * above. No lower bound on how negative shift can get (unlike
     * __fixsfsi's own `shift > -24` — that bound is about ITS overflow/
     * underflow saturation, not relevant here): 0.5f lands at exactly
     * shift == -24 (its entire mantissa, hidden bit included, is
     * fractional — no integer part at all), and excluding that boundary
     * here was a real, confirmed bug (0.5f printed "0.000000"). Smaller
     * magnitudes just mean more loop iterations in print_float_frac's
     * sf_shr calls below, not incorrectness.
     */
    int shift = aexp - SF_EXP_BIAS - SF_EXP_SHIFT;
    if (shift < 0) {
      pf_fbits_n = -shift;
      pf_frac_bits = aM & (sf_shl(1UL, pf_fbits_n) - 1UL);
    }
  }
}

/* Repeatedly does fixed-point "multiply the remaining fraction by 10,
 * take the integer part as the next digit" — the standard binary-
 * fraction-to-decimal technique, needing only 32-bit add/shift/mask
 * (all already proven safe throughout this file), never a float
 * multiply — prints all 6 digits print_float always produces. */
static void print_float_frac(void) {
  int i;
  for (i = 0; i < 6; i++) {
    u32 digit = 0;
    if (pf_fbits_n != 0) {
      pf_frac_bits = (pf_frac_bits << 3) + (pf_frac_bits << 1); /* *10 */
      digit = sf_shr(pf_frac_bits, pf_fbits_n);
      pf_frac_bits &= sf_shl(1UL, pf_fbits_n) - 1UL;
    }
    putchar('0' + (int)digit);
  }
}

/* Fixed-point, always 6 digits after the point — matching plain
 * printf("%f", ...)'s own C-standard default precision, since that's
 * the spelling this is standing in for (see the header comment above
 * this whole section). No exponent notation and no NaN/Inf special-
 * casing (this whole soft-float implementation doesn't represent NaN
 * specially — see __unordsf2 above) — very large magnitudes just
 * saturate the way __fixsfsi already does for %d today.
 */
void print_float(float f) {
  print_float_extract(f);
  print_float_frac();
}

/* Prints a 32-bit DG/IBM-style hex-format float given as the two
 * 16-bit words real hardware naturally hands it back in — the same
 * number format --hwfloat's own real Eclipse FAD/FAS/etc. instructions
 * use (sign + 7-bit excess-64 exponent over powers of 16, in the top
 * byte; 24-bit hex-normalized mantissa filling the rest — see
 * README.md's Floating Point Instructions section). `hi` holds bits
 * 31-16 (sign, exponent, and the top two hex digits of the mantissa);
 * `lo` holds bits 15-0 (the remaining four hex digits) — swap the two
 * arguments at the call site if a particular source hands them back
 * the other way around.
 *
 * Decodes and prints the hex-float bits DIRECTLY — never converts
 * through hexfloat32_to_ieee754/print_float the way an earlier version
 * of this function did. Both this and IEEE-754 boil down to the same
 * shape once you strip the format-specific packaging: a plain integer
 * significand times a power of two (`value = mant * 2^shift`, with
 * `shift = 4*(exponent_field-64) - 24` for a hex float, no separate
 * "hidden bit" trick needed since a hex-float mantissa's own leading
 * nonzero hex digit already IS what IEEE-754 keeps implicit). So this
 * reuses print_float_frac's decimal-digit-extraction loop just above
 * completely unchanged (it only ever consumed pf_frac_bits/pf_fbits_n,
 * genuinely agnostic to which float format they came from) — the only
 * new code here is deriving (integer part, pf_frac_bits, pf_fbits_n)
 * from the hex-float's own fields, the direct-decode equivalent of what
 * print_float_extract/__fixsfsi do for IEEE-754's fields.
 *
 * sf_shl/sf_shr (used exactly as print_float_extract already uses them)
 * are safe for any shift amount, including ones far outside IEEE-754's
 * own usual range — hex float's wider exponent means `shift` can run
 * roughly ±280 here vs. IEEE-754's ~±150, correspondingly more loop
 * iterations for an extreme-magnitude value, but no new bound/
 * saturation logic was needed: an oversized rightward shift just
 * converges to 0 (both for the integer part and, later, every decimal
 * digit print_float_frac extracts) exactly like a genuinely tiny value
 * should print as all zeros at 6-decimal-place precision, and an
 * oversized leftward shift saturates the same way a very large IEEE-754
 * magnitude already does through __fixsfsi's own existing behavior.
 */
void print_dg_float(unsigned int hi, unsigned int lo) {
  u32 bits = ((u32)hi << 16) | (u32)(lo & 0xFFFFUL);
  if (u32_and_nz(bits, 0x80000000UL)) {
    putchar('-');
  }
  u32 mant = bits & 0x00FFFFFFUL;
  pf_frac_bits = 0;
  pf_fbits_n = 0;
  if (u32_eq(mant, 0)) {
    print_uint32(0);
    putchar('.');
    print_float_frac();
    return;
  }
  int exp_field = (int)((bits >> 24) & 0x7FUL);
  int shift = 4 * (exp_field - 64) - 24;

  u32 uip;
  if (shift >= 0) {
    uip = sf_shl(mant, shift);
  } else {
    int nshift = -shift;
    uip = sf_shr(mant, nshift);
    pf_fbits_n = nshift;
    pf_frac_bits = mant & (sf_shl(1UL, nshift) - 1UL);
  }
  print_uint32(uip);
  putchar('.');
  print_float_frac();
}

/* --- math.h ---
 *
 * Built entirely on the soft-float primitives above (arithmetic,
 * comparison, int<->float conversion) rather than any new bit-level
 * mantissa/exponent surgery, on purpose: everything here already has
 * a proven-correct implementation to lean on, and this whole section
 * stays free of yet another set of SF_*_MASK-wrangling functions to
 * get right and verify from scratch.
 */

#define SF_ABS_MASK (SF_EXP_MASK | SF_MANT_MASK)

float fabsf(float f) { return sf_from_bits(sf_bits(f) & SF_ABS_MASK); }

/* (long)f truncates toward zero (see __fixsfsi above) -- floorf only
 * needs to correct the one case that disagrees with truncation:
 * negative and non-integral, where truncating rounds up (toward
 * zero) rather than down. Limited to values representable in a long
 * (see __fixsfsi's own comment on saturation for anything bigger),
 * same as every other int<->float conversion in this file.
 */
float floorf(float f) {
  long i = (long)f;
  float t = (float)i;
  if (f < t) {
    return t - 1.0f;
  }
  return t;
}

/* Mirror image of floorf: truncation already rounds the wrong way
 * (up, toward zero) for a *positive* non-integral value here.
 */
float ceilf(float f) {
  long i = (long)f;
  float t = (float)i;
  if (f > t) {
    return t + 1.0f;
  }
  return t;
}

/* Newton's method (x_{n+1} = (x_n + a/x_n) / 2), starting from x0 = a
 * itself -- globally convergent for any a > 0 regardless of starting
 * point, so correctness doesn't depend on a good initial guess, only
 * on enough iterations. A fixed, generous iteration count rather than
 * a convergence check: this target has no way to bail out of the loop
 * cheaply (every comparison here is itself a real function call --
 * see sf_cmp), and 32 iterations is comfortably enough for float's
 * 24-bit mantissa to converge from any *reasonable*-magnitude input
 * (this was not verified against extreme inputs, e.g. very large or
 * very small a, where x0 = a is a poor enough starting guess that
 * more warm-up iterations could be needed first).
 */
/* Named sf_sqrt, not sqrtf, and exposed as sqrtf only via a macro in
 * math.h -- calling this "sqrtf" directly hits a real, separate
 * backend bug: LLVM's TargetLibraryInfo recognizes any function
 * literally named sqrtf (matching libm's own signature) as *the*
 * standard sqrtf, regardless of clang-level -fno-builtin/-fno-math-
 * builtin (those are frontend flags; this recognition happens later,
 * inside llc's own middle-end, working off the compiled name alone)
 * -- and rewrites a call to it into a raw FSQRT node before this
 * function's own body is ever reached. This backend has no libcall
 * registered for FSQRT, so llc crashes outright ("LLVM ERROR:
 * unsupported library call operation"). Renaming so the literal name
 * "sqrtf" never appears in the compiled IR sidesteps the whole
 * problem with zero blast radius (unlike -disable-simplify-libcalls,
 * tried first and reverted: it does fix this, but it also disables
 * the *unrelated* comparison-lowering simplification floorf/ceilf's
 * own float comparisons depend on to avoid a similar ISel crash of
 * their own -- confirmed the hard way, by watching floorf break).
 * The same trick will likely be needed for every math.h function
 * added after this one that shares a name with a real libm function.
 */
float sf_sqrt(float a) {
  /* Guard via raw bits (u32_and_nz/u32_eq on sf_bits(a)), not a
   * direct "a > 0.0f" float comparison used as a branch condition --
   * confirmed the hard way that hits a real ISel crash here ("Cannot
   * select" on the brcond this function's own early-return produced).
   * Oddly inconsistent by name: floorf/ceilf's own "f < t"/"f > t"
   * branches compile fine, and standalone repros under some function
   * names crash while others (e.g. truncf) don't with the exact same
   * body -- never fully root-caused, and not worth blocking this on.
   * Sidestepping float-comparison-as-branch-condition entirely, via
   * the same u32_and_nz-on-the-sign-bit idiom sf_add_extract already
   * uses, avoids the whole question and is already proven safe
   * throughout this file's soft-float section.
   */
  u32 abits = sf_bits(a);
  if (u32_and_nz(abits, SF_SIGN_MASK)) {
    return 0.0f;
  }
  if (u32_eq(abits, 0UL)) {
    return 0.0f;
  }
  float x = a;
  int i;
  for (i = 0; i < 32; i++) {
    x = (x + a / x) * 0.5f;
  }
  return x;
}
