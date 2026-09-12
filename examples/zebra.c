#define DSKP_VARIANT_ZEBRA
#include "dskp_common.h"
#include "zebra.h"

/* See zebra.h's header comment before trusting any of this against
 * real Zebra hardware -- this has never been executed, only compiled
 * (confirmed against this project's real eclipse-cc + dgasm pipeline;
 * see ZEBRA_NOTES.md's "What was and wasn't checked" section, and
 * DSKP_FAMILY_NOTES.md for this file's post-unification verification).
 *
 * This file now sits on top of the shared DSKP-family register core
 * (dskp_common.h/.c): the bare DOA/DOC/DOB loads, bare DIA/DIB reads,
 * the P-pulsed Specify-Cylinder seek trigger, and the combined
 * S-pulsed Specify-Memory-Address+start+poll primitive are no longer
 * defined here -- they're identical across all three DSKP generations
 * and now live once in dskp_common.c. What stays HERE, because it's
 * real, documented Zebra-specific behavior, not incidental style:
 *
 *   - The DOA drive-select field is 2 bits (bits 9-10, drives 0-3) and
 *     the DOA "clear attention" field is 4 bits (bits 1-4, one bit per
 *     drive) -- Kismet's own DOA has a 1-bit drive-select field and a
 *     2-bit clear-flags field instead (see kismet.c); this is real,
 *     generation-specific DOA layout, not shared.
 *   - The "not-SEEK" DOC is a SINGLE word (surface + sector + count all
 *     fit directly in their 5-bit fields, no MSB split needed) --
 *     unlike Vulcan/Kismet, whose wider sector/count ranges need an
 *     extra "Specify Extended Sector and Count" DOC first. This is
 *     Zebra's own real simplification, not something to abstract away.
 *   - zebra_seek() explicitly polls this drive's own DIA attention bit
 *     after the P-pulsed seek, rather than relying on the controller's
 *     documented internal seek-deferral the way Vulcan/Kismet's drivers
 *     do -- a deliberate, conservative choice this file's original
 *     header comment explains at length (Appendix A's Programming
 *     Flowcharts are diagrams only, with no prose confirming exactly
 *     what "CONTROL BUSY"/"CONTROL FULL" individually gate, so this
 *     driver takes the unambiguous, if slower, explicit-wait path
 *     instead of the flowchart's own "FOLLOWUP R/W COMMAND?" shortcut).
 *   - Zebra's own extra DIB fault bits (Invalid Status, Reserved,
 *     Trespassed, Positioner Offset, Invalid Address, Illegal Command,
 *     Power Fault, Pack Unsafe, Clock Fault, Write Fault) beyond the
 *     five DIB bits dskp_common.h shares across the whole family.
 *   - The per-drive DIA attention bits (bits 2-5) and their DOA
 *     "clear attention" counterparts (bits 1-4) -- genuinely Zebra-only
 *     among the three (Vulcan/Kismet never poll for seek completion at
 *     all, so they have no equivalent use for their own DIA drive-done
 *     bits in this driver's scope).
 *   - Zebra's richer real capability beyond the shared core (explicit
 *     multi-sector burst count up to 32 sectors/operation, 8-word FIFO,
 *     32-bit ECC readback via the ALT MODE 1/2 registers, per-drive
 *     RESERVE/TRESPASS) is UNCHANGED by this refactor: still not
 *     implemented (single-sector, no ECC recovery, single-processor
 *     scope, exactly as before) -- see zebra.h's own scope comment.
 *     DSKP_CMD_ALT_MODE1/DSKP_CMD_ALT_MODE2, now shared family
 *     constants, are the entry points a future increment adding ECC
 *     readback would use.
 */

/* DOA per-drive "clear attention flag" bits (bits 1-4, DRV0-DRV3; p.
 * A-1: "1 CLEARS ATTENTION FLAG"). Zebra-specific -- see this file's
 * header comment. */
#define ZEBRA_DOA_CLR_ATTN_DRV0 0x4000u
#define ZEBRA_DOA_CLR_ATTN_DRV1 0x2000u
#define ZEBRA_DOA_CLR_ATTN_DRV2 0x1000u
#define ZEBRA_DOA_CLR_ATTN_DRV3 0x0800u

/* --- DIA bits 1-5: Zebra-specific (Control Full + per-drive attention),
 * on top of dskp_common.h's shared bits 6-15. See this file's header
 * comment and ZEBRA_NOTES.md on why bit 1's "CONTROL FULL" name is an
 * inference, not a direct confirmation. --- */
#define ZEBRA_DIA_CONTROL_FULL 0x4000u /* bit 1 */
#define ZEBRA_DIA_ATTN_DRV0    0x2000u /* bit 2 */
#define ZEBRA_DIA_ATTN_DRV1    0x1000u /* bit 3 */
#define ZEBRA_DIA_ATTN_DRV2    0x0800u /* bit 4 */
#define ZEBRA_DIA_ATTN_DRV3    0x0400u /* bit 5 */

/* --- DIB bits 0-2,5,7-11,13-14: Zebra-specific, on top of
 * dskp_common.h's shared DSKP_DIB_READY/BUSY/WRITE_DISABLED/
 * POSITIONER_FAULT/DRIVE_FAULT (bits 3,4,6,12,15). Bit 7 is shown
 * cross-hatched (reserved/unused) on Zebra's own diagram. --- */
#define ZEBRA_DIB_INVALID_STATUS  0x8000u /* bit 0 */
#define ZEBRA_DIB_DRIVE_RESERVED  0x4000u /* bit 1 */
#define ZEBRA_DIB_TRESPASSED      0x2000u /* bit 2 */
#define ZEBRA_DIB_POSITIONER_OFFSET 0x0400u /* bit 5 */
#define ZEBRA_DIB_INVALID_ADDRESS 0x0080u /* bit 8 */
#define ZEBRA_DIB_ILLEGAL_COMMAND 0x0040u /* bit 9 */
#define ZEBRA_DIB_POWER_FAULT     0x0020u /* bit 10 */
#define ZEBRA_DIB_PACK_UNSAFE     0x0010u /* bit 11 */
#define ZEBRA_DIB_CLOCK_FAULT     0x0004u /* bit 13 */
#define ZEBRA_DIB_WRITE_FAULT     0x0002u /* bit 14 */
/* Full fault-bit mask (bits 8-15), same set zebra_rw()/zebra_seek()
 * already checked before unification. */
#define ZEBRA_DIB_ANY_FAULT (ZEBRA_DIB_INVALID_ADDRESS | ZEBRA_DIB_ILLEGAL_COMMAND | \
    ZEBRA_DIB_POWER_FAULT | ZEBRA_DIB_PACK_UNSAFE | DSKP_DIB_POSITIONER_FAULT | \
    ZEBRA_DIB_CLOCK_FAULT | ZEBRA_DIB_WRITE_FAULT | DSKP_DIB_DRIVE_FAULT)

/* One and only one DSKP-family variant may be linked into a given
 * program -- see dskp_common.h's own comment on this symbol. */
int dskp_family_active_variant = DSKP_VARIANT_ID;

/* Arbitrary bounded retry count for the post-SEEK attention-flag poll
 * below -- see the original ZEBRA_SEEK_POLL_LIMIT comment (unchanged by
 * this refactor): must be unsigned long, not this backend's 16-bit
 * `unsigned int`, or the loop never terminates (a real bug eclipse-cc's
 * own diagnostics caught while this file was first written -- see
 * ZEBRA_NOTES.md). */
#define ZEBRA_SEEK_POLL_LIMIT 100000ul

/* Select `unit` with a NO OPERATION command and read back its DIB.
 * NOP is used rather than skipping the DOA entirely because DIB
 * appears to report on whichever drive the most recent DOA selected;
 * see the original comment on this choice in ZEBRA_NOTES.md/git
 * history -- unchanged by this refactor. */
unsigned int zebra_status(unsigned int unit) {
    unsigned int doa_word = DSKP_DOA_CMD(DSKP_CMD_NOP) | ((unit & 3u) << 9);
    dskp_doa(doa_word);
    return dskp_dib();
}

/* Per-drive DIA "attention" bit and DOA "clear attention" bit --
 * factored out since zebra_seek() needs both, keyed by unit. */
static unsigned int zebra_attn_bit(unsigned int unit) {
    switch (unit & 3u) {
        case 0: return ZEBRA_DIA_ATTN_DRV0;
        case 1: return ZEBRA_DIA_ATTN_DRV1;
        case 2: return ZEBRA_DIA_ATTN_DRV2;
        default: return ZEBRA_DIA_ATTN_DRV3;
    }
}
static unsigned int zebra_clr_attn_bit(unsigned int unit) {
    switch (unit & 3u) {
        case 0: return ZEBRA_DOA_CLR_ATTN_DRV0;
        case 1: return ZEBRA_DOA_CLR_ATTN_DRV1;
        case 2: return ZEBRA_DOA_CLR_ATTN_DRV2;
        default: return ZEBRA_DOA_CLR_ATTN_DRV3;
    }
}

/* Position the heads at `cylinder` on `unit`, and wait for that
 * drive's attention flag -- see this file's header comment on why
 * Zebra's driver takes this more conservative, explicitly-polled path
 * instead of Vulcan/Kismet's fire-and-forget one. Unchanged behavior
 * from before unification; now built on the shared dskp_doa/dskp_dib/
 * dskp_docp_seek/dskp_dia primitives instead of file-local copies of
 * them. */
static int zebra_seek(unsigned int unit, unsigned int cylinder) {
    unsigned int attn = zebra_attn_bit(unit);
    unsigned int doa_word, dib_status, dia_status;
    unsigned long tries;

    doa_word = DSKP_DOA_CMD(DSKP_CMD_SEEK) | ((unit & 3u) << 9);
    dskp_doa(doa_word);

    dib_status = dskp_dib();
    if (!(dib_status & DSKP_DIB_READY))
        return -3;

    dskp_docp_seek(cylinder);

    dia_status = 0;
    for (tries = 0; tries < ZEBRA_SEEK_POLL_LIMIT; tries++) {
        dia_status = dskp_dia();
        if (dia_status & attn)
            break;
    }
    if (!(dia_status & attn))
        return -2;

    /* Clear the attention flag we just consumed. */
    dskp_doa(zebra_clr_attn_bit(unit));

    dib_status = dskp_dib();
    if (!(dib_status & DSKP_DIB_READY))
        return -3;
    if (dib_status & ZEBRA_DIB_ANY_FAULT)
        return -4;
    return 0;
}

/* Shared core for both directions. is_write: 0 = read, nonzero =
 * write. Single sector only (surface/sector addressing, count = two's
 * complement of 1). Unchanged in behavior from before unification. */
static int zebra_rw(unsigned int unit, unsigned long blockno,
                     unsigned int *buf, int is_write) {
    unsigned long tmp;
    unsigned int cylinder, surface, sector;
    unsigned int doa_word, doc_word, dob_word;
    unsigned int count5;
    unsigned int dia_status, dib_status;
    int rc;

    if (unit > 3u || blockno >= ZEBRA_SECTORS_PER_DRIVE)
        return -1;

    /* Flat sector index -> cylinder/surface/sector -- this driver's own
     * invention, same convention vulcan.c uses (see its own comment). */
    sector = (unsigned int)(blockno % ZEBRA_SECTORS_PER_TRACK);
    tmp = blockno / ZEBRA_SECTORS_PER_TRACK;
    surface = (unsigned int)(tmp % ZEBRA_SURFACES_PER_UNIT);
    cylinder = (unsigned int)(tmp / ZEBRA_SURFACES_PER_UNIT);

    rc = zebra_seek(unit, cylinder);
    if (rc != 0)
        return rc;

    /* Select the drive again and store the READ/WRITE command -- DOC's
     * meaning is context-sensitive on the *previous* DOA (shared family
     * trait, dskp_common.h), so this DOA must be issued before the
     * surface/sector/count DOC below, to put DOC back into that
     * meaning. */
    doa_word = DSKP_DOA_CMD(is_write ? DSKP_CMD_WRITE : DSKP_CMD_READ) | ((unit & 3u) << 9);
    dskp_doa(doa_word);

    dib_status = dskp_dib();
    if (!(dib_status & DSKP_DIB_READY))
        return -3;

    /* Specify Surface, Sector and Count: bit 0 = MAP MODE (left 0),
     * bits 1-5 = surface (0-18), bits 6-10 = starting sector (0-23),
     * bits 11-15 = sector count as two's complement (all-ones = -1 =
     * one sector). Both fit their 5-bit fields directly -- Zebra's own
     * real simplification over Vulcan/Kismet's split fields, see this
     * file's header comment. */
    count5 = (unsigned int)(-1) & 0x1Fu;
    doc_word = ((surface & 0x1Fu) << 10)
             | ((sector  & 0x1Fu) << 5)
             | count5;
    dskp_doc(doc_word);

    /* Specify Memory Address (bit0 = EMA LSB, left 0) + S pulse to
     * start the transfer; dskp_dobs_start() polls SKPDN to completion. */
    dob_word = (unsigned int)(unsigned long)buf & 0x7FFFu;
    dskp_dobs_start(dob_word);

    /* Check status. DIA's error-bits field is returned directly to the
     * caller on a nonzero read, rather than collapsed to a single bit --
     * see zebra.h's return-value comment. */
    dia_status = dskp_dia();
    dib_status = dskp_dib();
    if (dib_status & ZEBRA_DIB_ANY_FAULT)
        return -4;
    if (DSKP_DIA_HAS_FAULT(dia_status))
        return (int)(dia_status & DSKP_DIA_ALLERR);

    return 0;
}

int zebra_read_block(unsigned int unit, unsigned long blockno, unsigned int *buf) {
    return zebra_rw(unit, blockno, buf, 0);
}

int zebra_write_block(unsigned int unit, unsigned long blockno, unsigned int *buf) {
    return zebra_rw(unit, blockno, buf, 1);
}
