#include <stdio.h>
#include "blockdev.h"
#include "dsk.h"

/* blockdev_test.c -- real, single-stepped-verifiable proof that
 * blockdev.h's generic dispatch (a struct of function pointers, backed
 * by a statically-initialized blockdev_t) actually works end to end on
 * this backend, going through blockdev_read()/blockdev_write() only --
 * dsk_read_block/dsk_write_block are never called directly from this
 * file. See disk_probe.s/STORAGE_NOTES.md for the original hand-
 * assembled proof this register sequence works at all; this file's job
 * is to prove the *generic interface on top of it* also works, through
 * the same three marker words (first, second, last of the 256-word
 * sector) that probe already used, so a passing run here is directly
 * comparable to that established baseline.
 */

static unsigned int wbuf[256];
static unsigned int rbuf[256];

/* One global blockdev_t, statically initialized to point at dsk.c's own
 * dsk_ops table. See blockdev.h's header comment for why this is a
 * two-level (blockdev_t -> blockdev_ops_t), two-fields-per-level shape
 * rather than one flat 3-field struct -- a flat struct compiled fine
 * but trapped at runtime (a real, now-documented backend limitation);
 * this shape is this project's own direct re-verification that the
 * *fixed* shape genuinely works, not just trust in DEBUGGING_NOTES.md's
 * entry #17 in isolation. */
blockdev_t dsk_dev = { &dsk_ops, 0 };

int main(void) {
    unsigned int blockno = 7;
    int wstatus, rstatus;
    int i;

    for (i = 0; i < 256; i++) {
        wbuf[i] = 0;
        rbuf[i] = 0;
    }
    wbuf[0] = 0123456;
    wbuf[1] = 0000377;
    wbuf[255] = 0177777;

    /* Both calls go through blockdev_write/blockdev_read -- i.e.
     * through dsk_dev.write_block(dsk_dev.ctx, ...) /
     * dsk_dev.read_block(dsk_dev.ctx, ...), a real indirect call
     * through a struct pointer's function-pointer field, dispatched
     * from a *separate* function (blockdev_write/blockdev_read in
     * blockdev.c) than the one holding the struct -- not inlined away
     * by construction (blockdev.c is compiled as part of the same
     * whole-program build, but the call still has to survive as a real
     * indirect JSR for this test to mean anything). */
    wstatus = blockdev_write(&dsk_dev, blockno, wbuf);
    rstatus = blockdev_read(&dsk_dev, blockno, rbuf);

    printf("wstatus=%o\n", wstatus);
    printf("rstatus=%o\n", rstatus);
    printf("rbuf0=%o\n", rbuf[0]);
    printf("rbuf1=%o\n", rbuf[1]);
    printf("rbuf255=%o\n", rbuf[255]);
    printf("wbuf0=%o\n", wbuf[0]);
    printf("wbuf1=%o\n", wbuf[1]);
    printf("wbuf255=%o\n", wbuf[255]);

    return 0;
}
