#ifndef _FPS_H
#define _FPS_H

/* Driver primitives for the Data General FPS100 floating-point
 * coprocessor, device code 054. Extracted from test_fps_add.c so any
 * FPS100 program can reuse the protocol without duplicating it.
 *
 * Device 054 uses a two-register command protocol distinct from the
 * standard Busy/Done handshake eclipse_io.h's outa/ina etc. implement:
 * every "write" is a bare (no S/C pulse, no wait loop) DOA of a command
 * word followed by a bare DOB of a data word -- confirmed by the
 * original hand-assembled test_fps_add.asm (known to work on real
 * hardware) never polling SKPDN/SKPBN around these, unlike TTI/TTO
 * elsewhere in this project.
 *
 * fpu_out/fpu_in are macros, not functions, specifically so every call
 * expands inline at its call site instead of going through JSR. The
 * original .asm writes value/cmd with LDA/LDA immediately followed by
 * DOA/DOB -- a tight, fixed-length instruction block with essentially
 * no gap between successive device operations. A real *function* call
 * on this backend carries a full prologue, epilogue, and JSR/return
 * around that same DOA/DOB pair, which is a lot more elapsed real time
 * between operations than the original ever had. This device's actual
 * timing tolerances aren't documented anywhere accessible (see
 * DEBUGGING_NOTES.md), so this isn't a confirmed-necessary fix -- it's
 * the one structural difference between a from-functions conversion and
 * a hand-assembled version *known* to work on real hardware that
 * eclipseemu can't help verify either way, since it doesn't simulate
 * device 054 at all. Inlining gets the instruction cadence back much
 * closer to the original's.
 */

#define FPU_DEV 054

#define fpu_out(cmd, value)                                                  \
    asm volatile(                                                            \
        "DOA %0,054\n\t"                                                     \
        "DOB %1,054\n\t"                                                     \
        :: "r"((unsigned int)(cmd)), "r"((unsigned int)(value)))

#define fpu_in(cmd)                                                          \
    __extension__({                                                          \
        unsigned int _fpu_r;                                                 \
        asm volatile(                                                        \
            "DOA %1,054\n\t"                                                 \
            "DIB %0,054\n\t"                                                 \
            : "=r"(_fpu_r) : "r"((unsigned int)(cmd)));                      \
        _fpu_r;                                                              \
    })

/* -- FPU commands (write to the switch/function register) -- */
#define cmd_wtsr 0021031  /* write switch register */
#define cmd_wtfn 0022031  /* write function register (act on switch reg) */
#define cmd_rdfn 0022030  /* read function/status register */
#define cmd_rdlt 0023030  /* read light/data register */

/* -- FPU function codes -- */
#define fn_load_tma  0001003
#define fn_load_ps_0 0001010
#define fn_load_ps_1 0001030
#define fn_load_ps_2 0001050
#define fn_load_ps_3 0001370
#define fn_start     0040000
#define fn_stop      0100000

#define fn_load_ma          0001002
#define fn_examine_regmd    0002015  /* declared, unused -- matches the original */
#define fn_examine_regmd_o1 0002035
#define fn_examine_regmd_o2 0002055
#define fn_examine_regmd_o3 0002075
#define fn_examine_regpsa   0002000
#define fn_examine_regtma   0002003

#endif
