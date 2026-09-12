#include "ulib.h"
#include "syscall.h"

/* __syscall: issues SYC with AC0=reason, AC1=arg1, AC2=arg2
 * (syscall.h's calling convention) and returns the value POPB leaves in
 * AC0 after examples/syscall_entry.s's handler runs.
 *
 * Follows examples/mmpu.c's own established, verified pattern for this
 * backend rather than "r"-constrained inline-asm operands: SYC's
 * calling convention is an ARCHITECTURAL convention (which AC holds
 * what) fixed by this project's own software choice, not an
 * operand-encoding one -- SYC's ACS/ACD fields carry no data at all
 * (TRAP_NOTES.md) -- exactly the same shape LMP's hardcoded AC0/AC1/
 * AC2 registers already have. mmpu.c's own header comment already
 * found that mixing that kind of hardcoded-by-convention register use
 * with "r"-constrained operands (which the register allocator is free
 * to place in AC0-AC2 for an unrelated reason) risks a collision this
 * backend's inline-asm implementation has "no verified way to
 * prevent (no clobber-list or fixed-register constraint support has
 * been confirmed to exist)". So, like mmpu_read_far/mmpu_write_far,
 * every value crosses this asm boundary through named globals
 * (ELDA/ESTA, referenced by their literal, unmangled C name), never
 * through an operand.
 *
 * AC2 save/restore, and why it's needed even though SYC/POPB's own
 * push/pop already preserves a caller's AC2 across the trap for free
 * (syscall.h's own header comment): this function ITSELF writes AC2
 * (to stage arg2) before SYC executes -- a self-inflicted clobber of
 * this function's OWN live frame pointer (this backend's calling
 * convention keeps it in AC2 for the whole function body, per
 * DEBUGGING_NOTES.md entry #11), unrelated to anything SYC does. The
 * value POPB hands back in AC2 after the trap is exactly what THIS
 * asm block put there just before SYC (arg2), not the frame pointer
 * this function's own compiled prologue (SAVE/MOV 3,2) established on
 * entry -- and this function's own epilogue (`return __sys_ret;`)
 * needs that real frame pointer again to store the return value at a
 * frame-relative slot before ITS OWN RTN. So AC2 has to be explicitly
 * saved before this block and restored after -- the identical
 * AC2-is-the-live-frame-pointer hazard examples/mmpu.c's
 * mmpu_read_far/mmpu_write_far already found and fixed for LMP, hit
 * again here for a different instruction (SYC) for the same underlying
 * reason. AC3 is saved/restored too, same "cheap insurance, not
 * confirmed live across this specific block but not confirmed safe to
 * skip either" reasoning mmpu.c gives for its own AC3 save/restore.
 */
static int __sys_reason;
static int __sys_arg1;
static int __sys_arg2;
static int __sys_ret;
static unsigned int __sys_save2;
static unsigned int __sys_save3;

static int __syscall(int reason, int arg1, int arg2) {
    __sys_reason = reason;
    __sys_arg1 = arg1;
    __sys_arg2 = arg2;
    /* Dummy writes so globaldce can't strip these globals' storage
     * entirely when only the invisible-to-it asm text below references
     * them for real -- same reasoning, same fix, as examples/mmpu.c's
     * own _mmpu_save2/_mmpu_save3 dummy writes. */
    __sys_save2 = 0;
    __sys_save3 = 0;
    asm volatile(
        "ESTA 2,__sys_save2,0\n\t" /* save the live frame pointer (AC2)
                                    *   and AC3 before clobbering either */
        "ESTA 3,__sys_save3,0\n\t"
        "ELDA 0,__sys_reason,0\n\t"
        "ELDA 1,__sys_arg1,0\n\t"
        "ELDA 2,__sys_arg2,0\n\t"
        "SYC 1,1\n\t"               /* NOT SYC 0,0 -- trap_probe.s's own
                                    *   AC0,AC0 push-skip special case */
        "ESTA 0,__sys_ret,0\n\t"    /* AC0 here is whatever
                                    *   syscall_entry.s's handler patched
                                    *   the pushed stack slot to, per
                                    *   TRAP_NOTES.md's verified fixup --
                                    *   read it before AC0 is disturbed
                                    *   by anything else below */
        "ELDA 2,__sys_save2,0\n\t" /* restore AC2/AC3 before falling
                                    *   back into compiler-generated code
                                    *   that assumes AC2 is still the
                                    *   frame pointer */
        "ELDA 3,__sys_save3,0\n\t"
    );
    return __sys_ret;
}

int sys_putchar(int c)        { return __syscall(SYS_PUTCHAR, c, 0); }
int sys_getchar(void)         { return __syscall(SYS_GETCHAR, 0, 0); }
int sys_blkread(int blockno)  { return __syscall(SYS_BLKREAD, blockno, 0); }
int sys_blkwrite(int blockno) { return __syscall(SYS_BLKWRITE, blockno, 0); }
