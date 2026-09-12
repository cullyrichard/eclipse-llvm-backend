#include "syscall.h"
#include "console.h"
#include "blockdev.h"
#include "dsk.h"

int sysarg1;
int sysarg2;

/* dsk_dev: the one block device this minimal kernel knows about --
 * matches examples/blockdev_test.c's own top-level setup exactly (same
 * &dsk_ops/0 construction). A kernel with more than one real backend
 * would index by a device number; out of scope here, matching this
 * ABI's own "minimal first set of syscalls" framing (SYSCALL_NOTES.md).
 */
blockdev_t dsk_dev = { &dsk_ops, 0 };

/* sysbuf: the fixed kernel-side buffer SYS_BLKREAD/SYS_BLKWRITE
 * transfer through, instead of a general user-supplied pointer. See
 * SYSCALL_NOTES.md's "What this syscall ABI does not attempt": a real
 * user-buffer copy would need to translate a user-mode logical pointer
 * through THAT process's own live map -- which is exactly the map that
 * is live-and-unsaved at the instant SYC first traps, and no longer
 * even selected by the time this handler runs (SYC's own dispatch
 * unconditionally disables Usermap, TRAP_NOTES.md) -- not something
 * this handler can dereference directly the way SYS_PUTCHAR's bare
 * character value can cross the boundary safely. Not attempted here. */
unsigned int sysbuf[DSK_WORDS_PER_SECTOR];

int syscall_dispatch(int reason) {
    /* Plain if/else-if, not switch -- this project has no confirmed
     * evidence a C `switch` statement lowers correctly on this backend
     * (no example anywhere in this codebase uses one), and every
     * dispatch in every prior phase (trap_probe.s's path_a/path_b,
     * syc_intmode_probe.s's h_informed/h_naive, scheduler_probe.s's
     * was_A/was_B) uses an explicit compare-and-branch chain instead --
     * matched here at the C level rather than introducing a new,
     * unverified construct into the one function this whole ABI funnels
     * through. */
    if (reason == SYS_PUTCHAR) {
        console_putchar(sysarg1);
        return 0;
    }
    if (reason == SYS_GETCHAR) {
        return console_getchar();
    }
    if (reason == SYS_BLKREAD) {
        return blockdev_read(&dsk_dev, (unsigned int)sysarg1, sysbuf);
    }
    if (reason == SYS_BLKWRITE) {
        return blockdev_write(&dsk_dev, (unsigned int)sysarg1, sysbuf);
    }
    return -1;
}

/* syscall_install: ELEF (Extended Load Effective address -- the same
 * "address of a symbol, not its value" instruction examples/mmpu.c's
 * LMP wrapper already uses for _mmpu_pte) computes __syscall_handler's
 * own address directly into AC0, sidestepping any question of whether
 * a `var NAME = someLabel` (address-of-a-label initializer) is valid
 * dgasm syntax for a var placed by examples/syscall_entry.s -- never
 * needed, since ELEF computes it at runtime instead. `STA 0,2` writes
 * it to location 2 (SC HANDLER ADDRESS) directly -- address 2 is a
 * literal, always inside page zero, so plain STA (not ESTA) is correct
 * here regardless of where __syscall_handler itself ends up. */
void syscall_install(void) {
    asm volatile(
        "ELEF 0,__syscall_handler,0\n\t"
        "STA 0,2\n\t"
    );
}
