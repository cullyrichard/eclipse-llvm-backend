#define DSKP_VARIANT_KISMET
#include "dskp_common.h"
#include "kismet.h"

/* kismet.c -- see kismet.h and KISMET_NOTES.md for the full picture.
 * This is UNVERIFIED against any running DSKP device: this project's
 * SIMH build does not model Kismet, so nothing here has been single-
 * stepped against real or simulated hardware. What *has* been checked
 * (see KISMET_NOTES.md's and DSKP_FAMILY_NOTES.md's verification
 * sections): this file compiles through `eclipse-cc`'s real pipeline
 * and the resulting assembly assembles cleanly with the real
 * `dgasm -t eclipse_s140`.
 *
 * This file now sits on top of the shared DSKP-family register core
 * (dskp_common.h/.c): the `dev DSKP = 027` declaration, the bare
 * DOA/DOC/DOB loads, bare DIA/DIB reads, the P-pulsed Specify-Cylinder
 * seek trigger, and the combined S-pulsed Specify-Memory-Address+
 * start+poll primitive are no longer defined here -- they're identical
 * across all three DSKP generations and now live once in
 * dskp_common.c. Two real, substantive changes from the pre-
 * unification version of this file, both explained here rather than
 * left silent:
 *
 *   1. REGISTER DISCIPLINE: this file previously routed every value
 *      through named globals (_k_doa_seek, _k_dib_status, etc.) and
 *      ELDA/ESTA, citing mmpu.c's rationale for why LMP-based code
 *      needs that. But this file's own original header comment already
 *      noted that reasoning is NARROWER than mmpu.c's: DOA/DOB/DOC/
 *      DIA/DIB/DIC (unlike LMP) accept whichever AC the compiler
 *      chooses, the same as zebra.c's and vulcan.c's own "r"-
 *      constrained inline-asm operands already relied on -- so there
 *      was never a hard technical reason Kismet's driver needed the
 *      global+ELDA/ESTA style, just a different (valid, but not
 *      required) choice made independently by this file's original
 *      author. Since dskp_common.c's shared dskp_doa/dskp_doc/dskp_dob/
 *      dskp_dia/dskp_dib primitives all use "r"-constrained operands
 *      (matching zebra.c/vulcan.c, needed so all three variants can
 *      share one implementation), this file now calls those directly
 *      instead of keeping its own global-based copies -- a real
 *      implementation change, not just a rename, but one this file's
 *      own prior comment already predicted was safe.
 *   2. DEVICE NAME REFERENCE: dskp_common.c's own instructions
 *      reference `DSKP` symbolically combined with a dynamic "r"-
 *      operand -- confirmed empirically to assemble correctly (a
 *      combination this file's pre-unification version, which only
 *      ever used `DSKP` with a fixed literal AC operand, had not
 *      actually tried -- see DSKP_FAMILY_NOTES.md).
 *
 * What stays HERE, unchanged, because it's real, documented Kismet-
 * specific behavior:
 *
 *   - The DOA drive-select field is 1 bit (bit 10 only, drives 0-1) and
 *     bit 9 must be 0 -- Zebra/Vulcan both use a 2-bit field (bits
 *     9-10, drives 0-3) instead; Kismet supports only 2 drives per
 *     controller (KISMET_UNIT_0/_1, kismet.h), a real hardware
 *     difference, not a simplification made by this driver.
 *   - The DOA "Clear Seek Done" field is 2 bits (bits 1-2, drives 0-1)
 *     -- vs. Zebra's/Vulcan's 4-bit "Clear Atten(0-3)" field, again
 *     tracking the 2-drive-vs-4-drive difference.
 *   - The "not-SEEK" case needs TWO DOCs, same shape as Vulcan's: a
 *     first "Specify Extended, Sector and Count" carrying MSBs, then a
 *     second "Specify Head, Sector and Count" carrying the low bits.
 *     Kismet's first DOC additionally carries a HEAD ADDRESS MSB (bit
 *     4) that Vulcan's equivalent DOC does not have -- needed because
 *     the 6214's 40 heads don't fit in the base 5-bit head field (bits
 *     1-5 of the second DOC), unlike Vulcan's fixed 19-surface geometry,
 *     which never needs a surface-address MSB at all.
 *   - No manual poll for seek completion -- same "fire and forget"
 *     strategy as Vulcan's driver (both generations' manuals document
 *     the controller deferring the stored read/write command
 *     internally until the outstanding seek finishes); Zebra alone
 *     polls explicitly. Kismet's own manual is in fact the most
 *     explicit of the three about this: "If a read/write operation is
 *     to follow, proceed immediately to Phase III without waiting for
 *     a drive attention interrupt request" (p.11-12, quoted in full in
 *     KISMET_NOTES.md).
 *   - The two genuine internal manual inconsistencies KISMET_NOTES.md
 *     documents (the DOC/DIC bit-1 double-listing between the "Head
 *     Address" 5-bit field and a separate "bit 1 reserved" prose row;
 *     and DIB alternate-mode-1's prose claiming an "extended head
 *     count in bit 4" that the same page's own bit table marks
 *     "Reserved") are UNCHANGED by this refactor -- this file's bit
 *     math still follows the field-boundary diagrams (self-consistent)
 *     over the prose tables' apparently-erroneous rows, exactly as
 *     before, and DSKP_FAMILY_NOTES.md repeats rather than re-resolves
 *     either inconsistency. Neither is exercised by this driver's
 *     read/write skeleton (the DIB alt-mode-1 field isn't read at all;
 *     the DOC bit-1/Head-Address ambiguity only matters for a head
 *     value using bit 1, which this driver's 5-bit head field does use
 *     for heads 2-3/6-7/etc. -- unresolved either way, same as before).
 */

/* One and only one DSKP-family variant may be linked into a given
 * program -- see dskp_common.h's own comment on this symbol. */
int dskp_family_active_variant = DSKP_VARIANT_ID;

/* Phase I only: select the drive, load the SEEK command, and read back
 * drive status without pulsing anything -- lets kismet_rw_op() decide
 * in plain C whether to proceed, before any seek or transfer starts.
 * Mirrors Figure 2's own "Phase I: Select a Drive and Specify a Seek
 * Command" flowchart (Programmer's Reference rev 1 p.11-12). */
static unsigned int kismet_select_and_check(int unit) {
    unsigned int doa_word = DSKP_DOA_CMD(DSKP_CMD_SEEK) | ((unsigned int)(unit & 1) << 10);
    dskp_doa(doa_word);          /* bare: no pulse yet */
    return dskp_dib();           /* bare: read status, don't clear */
}

/* Phases II-IV: start the seek (P pulse, does NOT touch the
 * controller's Busy/Done flags), immediately proceed to select the
 * drive + READ/WRITE command (per the manual's own "proceed
 * immediately to Phase III" guidance quoted above), load the extended
 * sector/count + head/sector/count + memory address registers, and
 * pulse S to start the transfer. Only the standard controller
 * Busy/Done flag (SKPDN DSKP, inside dskp_dobs_start()) is polled --
 * never DIB's per-drive Busy bit -- same generic Nova/Eclipse polled-
 * completion idiom STORAGE_NOTES.md's disk_probe.s already used for
 * DSK. Returns the final DIA status word. */
static unsigned int kismet_seek_and_xfer(int unit, int cmd, int cyl, int head,
                                          int sector, void *buf) {
    unsigned int head_msb = ((unsigned int)head >> 5) & 1u;
    unsigned int sector_msb = ((unsigned int)sector >> 5) & 1u;
    /* Two's complement of a 1-sector transfer in the 6-bit count field
     * (bit 10 of the 1st DOC = MSB, bits 11-15 of the 2nd DOC = low 5
     * bits): 64 - 1 = 63 decimal = 077 octal = all six bits set. */
    unsigned int count_msb = 1u;
    unsigned int count_low5 = 037u;
    unsigned int doa_rw, doc1, doc2, dob;

    dskp_docp_seek((unsigned int)cyl & 01777u); /* P pulse: starts the SEEK */

    doa_rw = DSKP_DOA_CMD((unsigned int)cmd) | ((unsigned int)(unit & 1) << 10);
    dskp_doa(doa_rw); /* bare: select drive + READ/WRITE cmd, per the
                        * manual proceed immediately, no wait for seek
                        * completion here */

    doc1 = (head_msb << 11) | (sector_msb << 10) | (count_msb << 5);
    dskp_doc(doc1); /* bare: Specify Extended, Sector and Count (1st) */

    doc2 = (((unsigned int)head & 037u) << 10) |
           (((unsigned int)sector & 037u) << 5) |
           (count_low5 & 037u);
    dskp_doc(doc2); /* bare: Specify Head, Sector and Count (2nd) */

    dob = (unsigned int)(unsigned long)buf & 077777u;
    dskp_dobs_start(dob); /* S pulse: sets mem addr AND starts the
                            * read/write, then polls SKPDN DSKP to
                            * completion */

    return dskp_dia(); /* bare: read final status, don't clear */
}

/* cmd values: kismet.h's own KISMET_CMD_* pre-unification constants
 * (000/002/016, pre-shifted into DOA bits 5-8) are replaced by the
 * shared, unshifted DSKP_CMD_READ/SEEK/WRITE from dskp_common.h,
 * shifted by DSKP_DOA_CMD() at the point of use above -- kismet_rw_op()
 * below now passes the raw (unshifted) command code. */

/* Shared by kismet_read_block/kismet_write_block -- see kismet.h for
 * the full contract (return value convention, CHS division). */
static int kismet_rw_op(int unit, int heads, unsigned int blockno,
                         void *buf, int cmd) {
    unsigned int sectors_per_cyl = (unsigned int)heads * KISMET_SECTORS_PER_TRACK;
    unsigned int cyl = blockno / sectors_per_cyl;
    unsigned int rem = blockno % sectors_per_cyl;
    unsigned int head = rem / KISMET_SECTORS_PER_TRACK;
    unsigned int sector = rem % KISMET_SECTORS_PER_TRACK;
    unsigned int dib_status;

    dib_status = kismet_select_and_check(unit);
    if (!(dib_status & KISMET_DIB_READY) ||
        (dib_status & KISMET_DIB_DRV_FAULT)) {
        return -1; /* not ready / faulted -- see kismet.h's return-value doc */
    }

    return (int)kismet_seek_and_xfer(unit, cmd, (int)cyl, (int)head, (int)sector, buf);
}

int kismet_read_block(int unit, int heads, unsigned int blockno, void *buf) {
    return kismet_rw_op(unit, heads, blockno, buf, DSKP_CMD_READ);
}

int kismet_write_block(int unit, int heads, unsigned int blockno, void *buf) {
    return kismet_rw_op(unit, heads, blockno, buf, DSKP_CMD_WRITE);
}
