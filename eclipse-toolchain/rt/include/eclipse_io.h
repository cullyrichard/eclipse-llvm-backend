#ifndef _ECLIPSE_IO_H
#define _ECLIPSE_IO_H

/* General device I/O, for any Nova/Eclipse device code -- generalizes
 * the exact idiom eclipse_rt.c's putchar/getchar already use for the
 * console (TTI/TTO). Mirrors eccc's own five builtins (docs/IO_DEVICES.md
 * in the sibling eccc project): in/out/io_done/io_busy/io_pulse.
 *
 * outa/outb/outc and ina/inb/inc are the A/B/C-channel data-transfer
 * pairs -- Nova/Eclipse instructions come in three channel flavors per
 * device (DOA/DIA, DOB/DIB, DOC/DIC), three separate data/control
 * registers a single device can expose. The CPU pseudo-device (077) is
 * this project's real B-channel user: Interrupt Acknowledge over DIB,
 * Mask Out over DOB.
 *
 * `device` (and IO_PULSE_*'s target) MUST be a compile-time constant.
 * Nova/Eclipse encodes the device code directly in the instruction word
 * -- there is no way to load a device code into a register and use it as
 * a runtime operand. These are macros, not functions, specifically so
 * the device code is pasted directly into the generated instruction
 * text at compile time; passing a variable will fail to assemble.
 *
 * IO_DONE/IO_BUSY synthesize a 0/1 result with no immediate-operand
 * instructions (this ISA has none at all -- see EclipseInstrInfo.td's
 * file header): `SUB %0,%0` zeroes the result via self-subtract, then
 * `INC %0,%0` (only reached if the skip-poll's JMP got skipped, i.e.
 * the flag *was* set) turns that 0 into 1 -- the same self-subtract and
 * skip-then-jump tricks already proven elsewhere in this backend.
 *
 * SKPDN/SKPBN (IO_DONE/IO_BUSY) and NIO/NIOS/NIOC/NIOP (IO_PULSE_*) take
 * a device code only -- confirmed against dgasm directly, which rejects
 * invented channel-letter spellings like `SKPDNB`/`SKPBNC`/`NIOB` as
 * unrecognised instructions. That matches real Nova/Eclipse hardware:
 * Busy/Done are per-*device* flags, and the control-pulse instructions
 * don't move data through any A/B/C register at all, so there is nothing
 * for a channel letter to select. Those four macros are already fully
 * generic and need no per-channel variants.
 *
 * NOTE on device 077 (the CPU pseudo-device): outb/inb do NOT work for
 * it -- confirmed empirically against eclipseemu: `DOBS 0,077` sets the
 * Busy flag but Done never asserts, so outb's wait-for-Done loop hangs
 * forever. Interrupt Acknowledge is reached via the CPU's hardware
 * interrupt-vector dispatch, not by polling Done. Device 077 must keep
 * using bare, non-blocking `asm volatile("DIB %0,077")` /
 * `asm volatile("DOB %0,077")` (no wait loop) the way this project's
 * interrupt handlers already do -- outb/inb are for genuine B-channel
 * *devices* that implement the standard Busy/Done handshake on that
 * register, not for 077.
 *
 * Device codes MUST be written in octal (leading zero: `011`, not `11`)
 * -- matches Nova/Eclipse's own convention (010=TTI, 011=TTO, 014=RTC,
 * 077=CPU, etc., per Appendix B's device code table) and dgasm's grammar
 * (leading '0' = octal, no leading zero = decimal). This is *not* just a
 * style preference: confirmed against a real dgasm run, `11` (no leading
 * zero) silently assembles as *decimal* 11 (dgasm octal 013) -- a
 * different, wrong device, with no error. Worse, `089` (looks like an
 * octal literal but 8/9 aren't valid octal digits) silently truncates to
 * device 0 -- also no error from dgasm.
 *
 * IO_CHECK_DEVICE_ below closes part of that gap: every macro forces
 * `device` through a genuine C constant-expression evaluation (not just
 * raw text substitution via `#device`), so a malformed octal literal
 * like `089` is rejected by the *C compiler itself* ("invalid digit '8'
 * in octal constant") before it ever reaches dgasm, and a `_Static_assert`
 * range-checks it to 0-077 (this ISA's actual 6-bit device-code field).
 * What this CANNOT catch: a caller who simply forgets the leading zero.
 * `11` and `011` are different C values (11 and 9) with no way for a
 * macro to recover "the digits you meant as octal" once the leading
 * zero is gone -- always double-check new device codes against
 * Appendix B before adding one.
 */
#define IO_CHECK_DEVICE_(device)                                             \
  _Static_assert((device) >= 0 && (device) <= 077,                           \
                  "device code must be 0-077 octal (this ISA's device "      \
                  "code field is 6 bits) -- and remember it must be "        \
                  "written with a leading zero to be read as octal at all")

/* Every device-taking macro below is defined through this extra layer
 * of indirection (outa calls outa_, never stringizes `device` itself
 * directly, etc.) on purpose: `#device` stringizes its argument *as
 * written*, without macro-expanding it first -- a classic preprocessor
 * gotcha. Without this indirection, a call like `outa(MY_DEVICE, x)`
 * where `MY_DEVICE` is itself a #define would paste the literal text
 * "MY_DEVICE" into the generated instruction instead of its value, and
 * dgasm would reject it as an undefined symbol (confirmed the hard way
 * against a real dgasm run). Routing through one more macro call first
 * lets ordinary argument substitution expand `device` *before* the inner
 * macro's `#device` stringizes it.
 */

/* outa(device, value): send `value` on the A channel, block until Done. */
#define outa(device, value) outa_(device, value)
#define outa_(device, value)                                                 \
  do {                                                                       \
    IO_CHECK_DEVICE_(device);                                                \
    asm volatile("DOAS %0," #device "\n\t"                                   \
                 "wait%=:\n\t"                                               \
                 "SKPDN " #device "\n\t"                                     \
                 "JMP wait%=\n\t" ::"r"((int)(value)));                      \
  } while (0)

/* outb(device, value): send `value` on the B channel, block until Done.
 * NOT valid for device 077 -- see the file header note above. */
#define outb(device, value) outb_(device, value)
#define outb_(device, value)                                                 \
  do {                                                                       \
    IO_CHECK_DEVICE_(device);                                                \
    asm volatile("DOBS %0," #device "\n\t"                                   \
                 "wait%=:\n\t"                                               \
                 "SKPDN " #device "\n\t"                                     \
                 "JMP wait%=\n\t" ::"r"((int)(value)));                      \
  } while (0)

/* outc(device, value): send `value` on the C channel, block until Done. */
#define outc(device, value) outc_(device, value)
#define outc_(device, value)                                                 \
  do {                                                                       \
    IO_CHECK_DEVICE_(device);                                                \
    asm volatile("DOCS %0," #device "\n\t"                                   \
                 "wait%=:\n\t"                                               \
                 "SKPDN " #device "\n\t"                                     \
                 "JMP wait%=\n\t" ::"r"((int)(value)));                      \
  } while (0)

/* ina(device): block until Done, read with the clear-pulse variant (plain
 * DIA leaves Done set, so a second read would return stale data). */
#define ina(device) ina_(device)
#define ina_(device)                                                         \
  __extension__({                                                            \
    IO_CHECK_DEVICE_(device);                                                \
    int _io_r;                                                               \
    asm volatile("wait%=:\n\t"                                              \
                 "SKPDN " #device "\n\t"                                     \
                 "JMP wait%=\n\t"                                            \
                 "DIAC %0," #device "\n\t"                                   \
                 : "=r"(_io_r));                                             \
    _io_r;                                                                   \
  })

/* inb(device): block until Done, read with the clear-pulse variant.
 * NOT valid for device 077 -- see the file header note above; use
 * bare `asm volatile("DIB %0,077")` for Interrupt Acknowledge instead. */
#define inb(device) inb_(device)
#define inb_(device)                                                         \
  __extension__({                                                            \
    IO_CHECK_DEVICE_(device);                                                \
    int _io_r;                                                               \
    asm volatile("wait%=:\n\t"                                              \
                 "SKPDN " #device "\n\t"                                     \
                 "JMP wait%=\n\t"                                            \
                 "DIBC %0," #device "\n\t"                                   \
                 : "=r"(_io_r));                                             \
    _io_r;                                                                   \
  })

/* inc(device): block until Done, read with the clear-pulse variant. */
#define inc(device) inc_(device)
#define inc_(device)                                                         \
  __extension__({                                                            \
    IO_CHECK_DEVICE_(device);                                                \
    int _io_r;                                                               \
    asm volatile("wait%=:\n\t"                                              \
                 "SKPDN " #device "\n\t"                                     \
                 "JMP wait%=\n\t"                                            \
                 "DICC %0," #device "\n\t"                                   \
                 : "=r"(_io_r));                                             \
    _io_r;                                                                   \
  })

/* io_done(device): 1 if the Done flag is currently set, 0 otherwise --
 * does not block. */
#define IO_DONE(device) IO_DONE_(device)
#define IO_DONE_(device)                                                     \
  __extension__({                                                            \
    IO_CHECK_DEVICE_(device);                                                \
    int _io_r;                                                               \
    asm volatile("SUB %0,%0\n\t"                                             \
                 "SKPDN " #device "\n\t"                                     \
                 "JMP io_done_end%=\n\t"                                     \
                 "INC %0,%0\n\t"                                             \
                 "io_done_end%=:\n\t"                                        \
                 : "+r"(_io_r));                                             \
    _io_r;                                                                   \
  })

/* io_busy(device): 1 if the Busy flag is currently set, 0 otherwise --
 * does not block. */
#define IO_BUSY(device) IO_BUSY_(device)
#define IO_BUSY_(device)                                                     \
  __extension__({                                                            \
    IO_CHECK_DEVICE_(device);                                                \
    int _io_r;                                                               \
    asm volatile("SUB %0,%0\n\t"                                             \
                 "SKPBN " #device "\n\t"                                     \
                 "JMP io_busy_end%=\n\t"                                     \
                 "INC %0,%0\n\t"                                             \
                 "io_busy_end%=:\n\t"                                        \
                 : "+r"(_io_r));                                             \
    _io_r;                                                                   \
  })

/* io_pulse(device, fn): fn selects the control pulse -- separate macros
 * rather than a runtime fn argument, for the same compile-time-constant
 * reason as `device` itself. */
#define IO_PULSE_NONE(device) IO_PULSE_NONE_(device)
#define IO_PULSE_NONE_(device)                                               \
  do {                                                                       \
    IO_CHECK_DEVICE_(device);                                                \
    asm volatile("NIO " #device);                                           \
  } while (0)
#define IO_PULSE_START(device) IO_PULSE_START_(device)
#define IO_PULSE_START_(device)                                              \
  do {                                                                       \
    IO_CHECK_DEVICE_(device);                                                \
    asm volatile("NIOS " #device);                                          \
  } while (0)
#define IO_PULSE_CLEAR(device) IO_PULSE_CLEAR_(device)
#define IO_PULSE_CLEAR_(device)                                              \
  do {                                                                       \
    IO_CHECK_DEVICE_(device);                                                \
    asm volatile("NIOC " #device);                                          \
  } while (0)
#define IO_PULSE_PULSE(device) IO_PULSE_PULSE_(device)
#define IO_PULSE_PULSE_(device)                                              \
  do {                                                                       \
    IO_CHECK_DEVICE_(device);                                                \
    asm volatile("NIOP " #device);                                          \
  } while (0)

#endif
