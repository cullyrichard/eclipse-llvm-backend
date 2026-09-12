#include "dsk.h"

/* Bare, unpulsed DOA/DOB for every register load, exactly as
 * disk_probe.s and STORAGE_NOTES.md establish for this device: DSK
 * triggers a transfer on ANY S/P pulse riding on ANY register-setting
 * instruction, regardless of which channel carried it
 * (`if (pulse) {...} if (pulse & 1) {transfer}` in nova_dsk.c, checked
 * independent of `code`). eclipse_io.h's outa/outb macros always emit
 * the combined S-pulsed form (DOAS/DOBS) and so are NOT safe to use
 * here -- see STORAGE_NOTES.md's "What a minimal block-driver API would
 * still need" section, which flags this exact gap. This file is that
 * gap closed: its own small dsk_set_blockaddr/dsk_set_memaddr/
 * dsk_read_status helpers (bare DOA/DOB/DIA) plus a separately-issued
 * bare NIOS/NIOP, mirroring vulcan.c's own dskp_doa/dskp_doc pattern
 * for the same "ordinary 'r'-constrained inline asm, no macro" reason.
 */

static void dsk_set_blockaddr(unsigned int blockno) {
    asm volatile("DOA %0,020" :: "r"(blockno));
}

static void dsk_set_memaddr(unsigned int addr) {
    asm volatile("DOB %0,020" :: "r"(addr));
}

/* Bare DIA -- does not clear status, matching disk_probe.s's own DIA
 * usage (a clearing read is never needed here: each call's status is
 * read exactly once, right after that same call's transfer completes,
 * never re-read afterward). */
static unsigned int dsk_read_status(void) {
    unsigned int status;
    asm volatile("DIA %0,020" : "=r"(status));
    return status;
}

int dsk_read_block(unsigned int blockno, unsigned int *buf) {
    if (blockno > DSK_MAX_BLOCK)
        return -1;

    dsk_set_blockaddr(blockno);
    dsk_set_memaddr((unsigned int)(unsigned long)buf);

    /* NIOS: pulse S, starts a READ (disk block -> buf). Separate bare
     * instruction, not combined with the DOB above -- see this file's
     * header comment.
     *
     * Label is a plain fixed name, not "%="-suffixed: found (and
     * confirmed with a minimal isolated repro, see BLOCKDEV_NOTES.md)
     * that this backend's inline-asm label substitution (`%=` -> a
     * unique per-instantiation number) only fires for an asm block that
     * has at least one real operand -- an operand-less asm block like
     * this wait loop leaves the literal text "%=" in the output, which
     * dgasm then rejects ("Unexpected character: '%'"). A plain fixed
     * label is safe here regardless: dsk_read_block is an ordinary
     * (non-static-inline, not force-inlined) function compiled once, so
     * its body -- and this label -- is emitted exactly once no matter
     * how many call sites invoke it. */
    asm volatile("NIOS 020");
    asm volatile(
        "dsk_read_wait:\n\t"
        "SKPDN 020\n\t"
        "JMP dsk_read_wait\n\t");

    return (int)dsk_read_status();
}

int dsk_write_block(unsigned int blockno, unsigned int *buf) {
    if (blockno > DSK_MAX_BLOCK)
        return -1;

    dsk_set_blockaddr(blockno);
    dsk_set_memaddr((unsigned int)(unsigned long)buf);

    /* NIOP: pulse P, starts a WRITE (buf -> disk block). Fixed label,
     * same reason as dsk_read_block's wait loop above. */
    asm volatile("NIOP 020");
    asm volatile(
        "dsk_write_wait:\n\t"
        "SKPDN 020\n\t"
        "JMP dsk_write_wait\n\t");

    return (int)dsk_read_status();
}

int dsk_blockdev_read(void *ctx, unsigned int blockno, void *buf) {
    (void)ctx;
    return dsk_read_block(blockno, (unsigned int *)buf);
}

int dsk_blockdev_write(void *ctx, unsigned int blockno, void *buf) {
    (void)ctx;
    return dsk_write_block(blockno, (unsigned int *)buf);
}

/* dsk_ops: statically-initialized 2-field blockdev_ops_t (both fields
 * "first"/"last" of two, per blockdev.h's own header comment on why
 * that matters on this backend) naming the two adapters above. */
const blockdev_ops_t dsk_ops = { dsk_blockdev_read, dsk_blockdev_write };
