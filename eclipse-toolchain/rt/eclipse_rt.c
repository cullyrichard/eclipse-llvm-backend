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

int printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = 0;
  while (*fmt) {
    if (*fmt == '%') {
      fmt++;
      if (*fmt == 'd') {
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
 * long` throughout: this target has no i32 multiply/divide either (same
 * README limitation, `__mulsi3`/`__udivsi3` don't exist), so using them
 * here would just trade one missing runtime symbol for another. Multiply
 * and divide are both done as manual bit-loops using only shifts,
 * compares, and add/subtract, which *do* legalize natively (wide
 * integer comparison/add/sub decompose cleanly into 16-bit half
 * operations; multiply and divide don't, which is exactly why they need
 * a libcall in the first place).
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
