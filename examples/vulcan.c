#define DSKP_VARIANT_VULCAN
#include "dskp_common.h"
#include "vulcan.h"

/* See vulcan.h's header comment before trusting any of this against
 * real Vulcan hardware -- this has never been executed, only compiled
 * (see DSKP_FAMILY_NOTES.md for this file's post-unification
 * verification).
 *
 * This file now sits on top of the shared DSKP-family register core
 * (dskp_common.h/.c): the bare DOA/DOC/DOB loads, bare DIA/DIB reads,
 * the P-pulsed Specify-Cylinder seek trigger, and the combined
 * S-pulsed Specify-Memory-Address+start+poll primitive are no longer
 * defined here -- they're identical across all three DSKP generations
 * and now live once in dskp_common.c. What stays HERE, because it's
 * real, documented Vulcan-specific behavior:
 *
 *   - The DOA drive-select field is 2 bits (bits 9-10, drives 0-3),
 *     same width/position as Zebra's, but Kismet's is only 1 bit (see
 *     kismet.c) -- kept per-file since it's not shared by all three.
 *   - The "not-SEEK" case needs TWO DOCs: a first "Specify Extended
 *     Sector and Count" carrying the sector-address MSB (bit 5) and
 *     sector-count MSB (bit 10), then a second "Specify Surface,
 *     Sector and Count" carrying MAP/surface(5b)/sector-low5/
 *     count-low5 -- because 35 sectors/track and a 64-sector max count
 *     don't fit in Zebra's narrower 5-bit fields. This is the
 *     "sector/sector-count fields split 1 MSB + 5 LSB across two
 *     separate DOC writes" subtlety VULCAN_NOTES.md flags explicitly as
 *     easy to get wrong -- preserved exactly as before unification.
 *   - No manual poll for seek completion between the seek and the
 *     read/write phases -- Vulcan's own manual explicitly documents the
 *     controller itself deferring the stored read/write command
 *     internally until the outstanding seek finishes (quoted in full in
 *     VULCAN_NOTES.md), so this driver relies on that guarantee instead
 *     of Zebra's explicit per-drive attention-bit poll. Structurally
 *     the same choice Kismet's driver makes (see kismet.c) -- Vulcan and
 *     Kismet share this "fire and forget" seek strategy; Zebra alone
 *     takes the more conservative, explicitly-polled path.
 *   - The coarse, single-summary-bit fault check (DIA bit 15 OR DIB bit
 *     15) is unchanged -- see vulcan.h's return-value comment for why a
 *     fuller version would need more than this skeleton returns.
 */

/* One and only one DSKP-family variant may be linked into a given
 * program -- see dskp_common.h's own comment on this symbol. */
int dskp_family_active_variant = DSKP_VARIANT_ID;

/* DIA bit 0: Control Full -- Vulcan-specific bit POSITION (Zebra's own
 * manual places the analogous bit at bit 1 instead, an inferred name at
 * that -- see ZEBRA_NOTES.md; Kismet's manual agrees with Vulcan's bit
 * 0 position, see kismet.c). Not part of dskp_common.h's shared bits
 * for exactly that reason -- the three generations don't agree on this
 * one bit's position, only on bits 6-15. */
#define VULCAN_DIA_CONTROL_FULL 0x8000u

/* Shared core for both directions. is_write: 0 = read, nonzero = write.
 * Unchanged in behavior from before unification. */
static int vulcan_rw(unsigned int unit, unsigned long blockno,
                      unsigned int *buf, int is_write) {
    unsigned long cyl_and_surf;
    unsigned int cyl, surface, sector;
    unsigned int sector6, count6;
    unsigned int doa_word, doc1_word, doc2_word, dob_word;
    unsigned int dia_status, dib_status;

    if (unit > 3u || blockno >= DSKP_SECTORS_PER_DRIVE)
        return -1;

    /* Flat sector index -> cylinder/surface/sector, same auto-increment
     * order the manual itself describes (pgmref.pdf p.4, quoted in
     * VULCAN_NOTES.md). */
    sector = (unsigned int)(blockno % DSKP_SECTORS_PER_SURF);
    cyl_and_surf = blockno / DSKP_SECTORS_PER_SURF;
    surface = (unsigned int)(cyl_and_surf % DSKP_SURFACES_PER_CYL);
    cyl = (unsigned int)(cyl_and_surf / DSKP_SURFACES_PER_CYL);

    /* --- Phase I: select a drive and specify a seek command
     * (pgmref.pdf p.11). Single-processor simplification per the
     * manual's own note -- only the Ready bit is checked below. */
    while (dskp_dia() & VULCAN_DIA_CONTROL_FULL)
        ;
    doa_word = DSKP_DOA_CMD(DSKP_CMD_SEEK) | ((unit & 3u) << 9);
    dskp_doa(doa_word);

    dib_status = dskp_dib();
    if (!(dib_status & DSKP_DIB_READY))
        return -2;

    /* --- Phase II: position the heads. --- */
    dskp_docp_seek(cyl);

    /* --- Phase III: select a drive and specify a read/write command
     * (pgmref.pdf p.12). --- */
    while (dskp_dia() & VULCAN_DIA_CONTROL_FULL)
        ;
    doa_word = DSKP_DOA_CMD(is_write ? DSKP_CMD_WRITE : DSKP_CMD_READ) | ((unit & 3u) << 9);
    dskp_doa(doa_word);

    /* --- Phase IV: specify sector/surface/count, then memory address
     * + S pulse to start the transfer (pgmref.pdf p.12). One sector
     * only: starting sector = `sector` (not two's complement -- only
     * the *count* field is two's complement), count = two's complement
     * of 1, i.e. all-ones in the 6-bit field. */
    sector6 = sector & 0x3Fu;
    count6 = (unsigned int)(-1) & 0x3Fu;

    /* First DOC ("Specify Extended Sector and Count", pgmref.pdf p.6):
     * bit5 = sector address MSB, bit10 = sector count MSB. Must
     * precede the second DOC per the manual's own NOTE on that page --
     * the real subtlety VULCAN_NOTES.md flags. */
    doc1_word = (((sector6 >> 5) & 1u) << 10)
              | (((count6  >> 5) & 1u) << 5);
    dskp_doc(doc1_word);

    /* Second DOC ("Specify Surface, Sector and Count", pgmref.pdf p.6):
     * bit0 = MAP (left 0), bits1-5 = surface, bits6-10 = sector low 5
     * bits, bits11-15 = count low 5 bits. */
    doc2_word = ((surface & 0x1Fu) << 10)
              | ((sector6  & 0x1Fu) << 5)
              | (count6 & 0x1Fu);
    dskp_doc(doc2_word);

    /* DOB ("Specify Memory Address", pgmref.pdf p.7): bit0 = extended
     * memory address LSB (left 0), bits1-15 = memory address. Combined
     * with the S pulse that starts the transfer. */
    dob_word = (unsigned int)(unsigned long)buf & 0x7FFFu;
    dskp_dobs_start(dob_word);

    /* --- Check status: a coarse OR of each register's single summary
     * fault bit -- see vulcan.h's return-value comment. --- */
    dia_status = dskp_dia();
    dib_status = dskp_dib();
    if ((dia_status & DSKP_DIA_RW_FAULT) || (dib_status & DSKP_DIB_DRIVE_FAULT))
        return -3;

    return 0;
}

int vulcan_read_block(unsigned int unit, unsigned long blockno, unsigned int *buf) {
    return vulcan_rw(unit, blockno, buf, 0);
}

int vulcan_write_block(unsigned int unit, unsigned long blockno, unsigned int *buf) {
    return vulcan_rw(unit, blockno, buf, 1);
}
