#ifndef _SYSCALL_H
#define _SYSCALL_H

/* syscall.h -- the kernel-side half of this project's syscall ABI: a
 * small, fixed table of syscall numbers dispatched by AC0, sitting on
 * top of TRAP_NOTES.md's already-verified SYC mechanism. Pairs with:
 *
 *   - examples/syscall_entry.s: hand-written Eclipse S/140 assembly,
 *     installed at memory location 2 (SC HANDLER ADDRESS -- Table 2.14,
 *     TRAP_NOTES.md) via syscall_install() below. This is the actual
 *     SYC trap target and CANNOT be an ordinary compiled C function --
 *     TRAP_NOTES.md's own "What's open" section already flagged why: a
 *     normal function's compiler-generated SAVE-based prologue / RTN
 *     epilogue is the wrong shape for a hardware jump target that must
 *     return via POPB, not RTN, and reads its own arguments out of
 *     AC0-AC2 directly rather than a pushed frame. So the raw trap
 *     entry/exit stays hand-written, exactly like every prior SYC probe
 *     (trap_probe.s, syc_intmode_probe.s) -- what syscall_entry.s adds
 *     beyond those two is a real syscall table (this file's SYS_*
 *     numbers) and a single, genuinely reusable hand-off into compiled,
 *     already-verified kernel C code (console.c/blockdev.c/dsk.c) via
 *     syscall_dispatch() below, instead of hand-writing the I/O work in
 *     assembly too.
 *   - examples/ulib.h/ulib.c: the USER-MODE-side C wrappers
 *     (sys_putchar/sys_getchar/sys_blkread/sys_blkwrite) that issue SYC
 *     with this file's own numbers and convention.
 *
 * ---- Calling convention ----
 * AC0 = syscall number (one of the SYS_* values below), input only.
 * AC1 = arg1, AC2 = arg2 -- both input only, both optional per syscall
 *   (an unused arg is passed as 0). This extends TRAP_NOTES.md's own
 *   trap_probe.s convention (AC0=reason, AC1=one argument) by one more
 *   register -- safe to do because SYC pushes, and POPB restores,
 *   AC0-AC3 as a single block regardless of how many of them a given
 *   handler actually uses (TRAP_NOTES.md: "AC2/AC3 pass through
 *   automatically... a future C-callable wrapper gets the compiler's
 *   live frame pointer (AC2) preserved through the trap for free") --
 *   this ABI is the first thing in this project to actually spend that
 *   already-verified headroom, rather than just note it exists.
 * AC0 = return value (output) -- the same "patch the stack's saved AC0
 *   slot before POPB" mechanism trap_probe.s/syc_intmode_probe.s
 *   already established and verified; syscall_entry.s does the same
 *   fixup around a real call to syscall_dispatch() instead of inline
 *   handler logic.
 *
 * A caller's own AC2 (its live compiled frame pointer) survives a SYC
 * round trip for free on SYC's own account -- the hardware push/pop
 * covers it like any other AC. examples/ulib.c's __syscall() still
 * explicitly saves/restores AC2 around its own asm block anyway; that
 * is for a different, self-inflicted reason (it has to WRITE AC2 with
 * arg2's value before SYC executes) -- see its own header comment.
 */

#define SYS_PUTCHAR   0   /* arg1 = character to write. returns 0. */
#define SYS_GETCHAR   1   /* no args. returns the character read. */
#define SYS_BLKREAD   2   /* arg1 = block number. returns dsk status (0
                             = success); data lands in the kernel's own
                             fixed scratch buffer, sysbuf -- see
                             syscall.c and SYSCALL_NOTES.md's "What this
                             syscall ABI does not attempt" for why a
                             general user-buffer copy is out of scope. */
#define SYS_BLKWRITE  3   /* arg1 = block number. returns dsk status (0
                             = success); writes FROM sysbuf. */

/* sysarg1/sysarg2: syscall_entry.s stashes the trapped AC1/AC2 here
 * (captured before anything else, including DIA, can disturb them)
 * before calling syscall_dispatch(reason) below. Kept as plain global
 * ints -- not passed as extra C parameters -- so the hand-written-
 * asm-to-compiled-C call itself stays the single-int-argument shape
 * this project has actually verified a hand-written call into compiled
 * C use (eclipse-toolchain/rt/eclipse_hwfloat.s's EJSR calls, all
 * either 1 or 2 arguments), rather than assuming an unverified
 * 3-argument shape works by extrapolation. See SYSCALL_NOTES.md. */
extern int sysarg1;
extern int sysarg2;

/* syscall_dispatch: the one C-callable entry point syscall_entry.s's
 * hand-written asm calls (via EJSR). `reason` is the trapped AC0
 * (captured by syscall_entry.s before DIA/anything else can clobber
 * it); sysarg1/sysarg2 above carry the rest. Returns the value that
 * becomes the caller's post-SYC AC0. This IS the whole syscall table --
 * dispatches on `reason`; anything not in SYS_* above returns -1. */
int syscall_dispatch(int reason);

/* syscall_install: installs __syscall_handler (examples/syscall_entry.s)
 * as location 2's SC HANDLER ADDRESS. An ordinary compiled C function
 * (its own SAVE/RTN prologue/epilogue is fine here -- installing the
 * vector is just two instructions, nothing about POPB/hardware-jump-
 * target semantics applies to THIS function itself). Must be called
 * once, early, before the first SYC any code in the program issues. */
void syscall_install(void);

#endif
