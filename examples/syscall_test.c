#include <stdio.h>
#include "ulib.h"
#include "syscall.h"

/* syscall_test.c -- compiled-C smoke test for the syscall ABI
 * (examples/syscall.h/.c, examples/syscall_entry.s, examples/ulib.h/.c)
 * end to end, through the real eclipse-cc-style pipeline: install the
 * SYC handler, then exercise all four syscalls purely through ulib.c's
 * user-callable wrappers -- no hand-written inline asm in this file at
 * all, proving a C program really can call e.g. sys_putchar('A')
 * without hand-writing SYC itself.
 *
 * Deliberately run from ordinary SUPERVISOR-mode context (this program
 * never enables Usermap) -- this test's job is to prove the syscall
 * ABI's C-level plumbing (calling convention, dispatch, the real
 * console/blockdev kernel work) all work correctly through the full
 * compiled toolchain. Genuine USER-mode SYC (the MapIntMode-recovery
 * discipline, and its interaction with a live scheduler timer) is
 * separately verified by examples/syscall_user_probe.s and
 * examples/syscall_sched_race_probe.s, hand-assembled the same way
 * TRAP_NOTES.md's own syc_intmode_probe.s is -- see SYSCALL_NOTES.md
 * for why a genuine user-mode-entry version of THIS specific compiled
 * program (which would need its entire logical footprint identity-
 * mapped) was not pursued as a second copy of the same coverage.
 *
 * sysbuf is peeked/poked directly here (via the extern below) purely
 * as test-harness setup/verification -- legitimate only because this
 * test never leaves supervisor mode, so there is no user/kernel
 * boundary here for that to violate; ordinary user-mode code has no
 * business (and, once addressed by SYSCALL_NOTES.md's own limitations
 * section, no ability) to do this.
 */

extern unsigned int sysbuf[];

int main(void) {
    syscall_install();

    sys_putchar('H');
    sys_putchar('I');
    sys_putchar('\n');

    int gotc = sys_getchar();
    sys_putchar(gotc);
    sys_putchar('\n');

    sysbuf[0] = 042424;   /* octal marker */
    sysbuf[1] = 012121;
    int wstatus = sys_blkwrite(7);

    sysbuf[0] = 0;        /* clobber before reading back, so a no-op
                            * blkread can't accidentally look correct */
    sysbuf[1] = 0;
    int rstatus = sys_blkread(7);

    /* Printed via rt/'s own printf -- test-harness output only, not
     * part of the syscall path under test. */
    printf("wstatus=%d rstatus=%d rbuf0=%o rbuf1=%o\n",
           wstatus, rstatus, sysbuf[0], sysbuf[1]);

    return 0;
}
