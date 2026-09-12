#ifndef _ULIB_H
#define _ULIB_H

/* ulib.h -- the USER-MODE side of this project's syscall ABI: ordinary,
 * user-callable C wrappers around the real SYC instruction, so a
 * user-mode C program never hand-writes SYC/AC0-AC2 setup or the
 * POPB-return-value-unwrapping trick itself. Pairs with
 * examples/syscall.h (the shared syscall numbers/convention) and
 * examples/syscall_entry.s (the kernel-side handler these wrappers
 * trap into).
 *
 * ---- The user/kernel C runtime split this project didn't have yet ----
 * Before this file, eclipse-toolchain/rt/ was THE one C runtime every
 * eclipse-cc-compiled program links against, with no distinction
 * between "code running in supervisor mode with direct hardware
 * access" and "code running in user mode that must go through a
 * syscall for I/O." This file is the other half of that split:
 *   - eclipse-toolchain/rt/ (unchanged, not touched by this work):
 *     stays exactly what it always was -- the supervisor-mode/
 *     direct-hardware-access runtime (printf/scanf/putchar/getchar/
 *     soft-float/etc., all touching TTI/TTO or other devices directly).
 *     A kernel's own internals (syscall.c included) still use it or
 *     console.c/dsk.c/blockdev.c directly -- those already run in
 *     supervisor mode by construction (SYC's own dispatch disables
 *     Usermap before the handler's first instruction, TRAP_NOTES.md).
 *   - ulib.c (this file): the user-mode runtime. Every function here
 *     assumes Usermap may be genuinely enabled and does ZERO direct
 *     device I/O -- every one of them is a thin SYC-issuing wrapper,
 *     nothing else. A user-mode C program links against THIS, not
 *     rt/'s putchar/getchar, for I/O -- calling rt/'s putchar from
 *     real user mode would try to touch the TTO device register
 *     directly, which is exactly the kind of access a real OS's user/
 *     kernel boundary exists to prevent (this project doesn't yet
 *     enforce that via I/O protection/MMPU faulting -- see
 *     SYSCALL_NOTES.md's "What this syscall ABI does not attempt" --
 *     but ulib.c's own existence is the ABI-level half of that
 *     boundary: user code that only ever calls ulib.c's wrappers never
 *     needs direct device access in the first place).
 *
 * Naming: sys_* rather than POSIX's open/read/write -- a clean, minimal
 * ABI matched to this project's actual verified kernel surface
 * (console + one block device), not an attempt at POSIX compatibility.
 */

int sys_putchar(int c);
int sys_getchar(void);
int sys_blkread(int blockno);
int sys_blkwrite(int blockno);

#endif
