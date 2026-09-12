#include "zebra.h"

/* See zebra.h's header comment before trusting any of this against
 * real Zebra hardware -- this has never been executed, only compiled
 * (confirmed against this project's real eclipse-cc + dgasm pipeline;
 * see ZEBRA_NOTES.md's "What was and wasn't checked" section).
 *
 * Register-load instructions (DOA/DOC, bare, no pulse) all have a
 * genuine per-instruction accumulator operand, so this file uses
 * ordinary "r"-constrained inline-asm operands throughout -- the same
 * pattern examples/fps.h's fpu_out/fpu_in and examples/vulcan.c use,
 * and unlike examples/mmpu.c's LMP-based code, which has to route
 * everything through named globals instead (see mmpu.c's own header
 * comment for why). The device select code (027 octal) is pasted
 * directly as literal text in each asm string, exactly as vulcan.c and
 * fps.h do for their own devices -- this sidesteps any question of
 * whether dgasm accepts a symbolic `dev NAME = code` name inside a
 * %-operand-substituted instruction; a bare octal literal in the
 * mnemonic's device-code slot is definitely accepted (every existing
 * driver in this project does that for at least one instruction, and
 * this file's own successful compile -- see ZEBRA_NOTES.md -- confirms
 * it for DSKP specifically).
 *
 * Pulse discipline: the Technical Manual's "S, C AND P FUNCTIONS" note
 * (Chapter I "Programming Summary", repeated near-verbatim as IORESET/
 * BUSY/DONE/IOSTART/IOCLEAR/IOPULSE in Appendix A p. A-1) documents the
 * flag commands as acting on the *device*, not on whichever specific
 * DOA/DOB/DOC instruction happened to carry the pulse -- the same
 * behavior examples/disk_probe.s and STORAGE_NOTES.md found (by
 * reading nova_dsk.c directly) for DSK's simulated hardware, and the
 * same choice examples/vulcan.c made for its closely related DSKP
 * device (see zebra.h's header comment on the relationship between the
 * two). Register loads that the Programming Flowcharts (Appendix A pp.
 * A-3/A-4) show issued together with a flag command (DOCP to seek,
 * "DOB + S" to start a transfer) are written here as ONE combined
 * register-load+pulse instruction (DOCP / DOBS) -- proven to assemble
 * by examples/vulcan.c already, and reconfirmed by this file's own
 * successful compile. Every *other* register load (the drive-select/
 * command DOA, and the surface/sector/count DOC) is issued bare,
 * unpulsed, mirroring disk_probe.s's and vulcan.c's own stated reason:
 * on these DG disk-storage-subsystem devices, a pulse riding on ANY
 * register-setting instruction triggers the associated flag command,
 * so a multi-register command sequence must keep the pulse off every
 * step but the last.
 */

/* --- Command codes: DOA bits 5-8 (Technical Manual Appendix A,
 * "Instruction Summary", DOA-DSKP accumulator format, p. A-1 of 44 --
 * cross-checked byte-for-byte against the same, lower-resolution
 * diagram on the unpaginated Chapter I "Programming Summary" page,
 * Rev. 01 vs. Rev. 02 of the same manual). Identical list, at the same
 * bit position, to examples/vulcan.c's DSKP_CMD_* table for the later
 * Model 6122 controller -- see zebra.h's header comment. --- */
#define ZEBRA_CMD_READ          000
#define ZEBRA_CMD_RECAL         001
#define ZEBRA_CMD_SEEK          002
#define ZEBRA_CMD_STOP          003
#define ZEBRA_CMD_OFFSET_FWD    004
#define ZEBRA_CMD_OFFSET_REV    005
#define ZEBRA_CMD_WRITE_DISABLE 006
#define ZEBRA_CMD_RELEASE       007
#define ZEBRA_CMD_TRESPASS      010
#define ZEBRA_CMD_ALT_MODE1     011
#define ZEBRA_CMD_ALT_MODE2     012
#define ZEBRA_CMD_NOP           013
#define ZEBRA_CMD_VERIFY        014
#define ZEBRA_CMD_READ_BUFFERS  015
#define ZEBRA_CMD_WRITE         016
#define ZEBRA_CMD_FORMAT        017

/* DOA bit 0: best-effort "Clear R/W Done". Zebra's own Appendix A
 * instruction-summary diagram labels this bit position tersely "R/W"
 * with no accompanying prose anywhere in its 44 pages (Appendix A is
 * diagrams only). examples/vulcan.c's closely related DSKP device sets
 * the same bit position unconditionally on every command issue,
 * commented there as "Clear R/W Done" -- taken directly from that
 * device's own (more legible) manual. Borrowed here by analogy, not
 * independently confirmed against Zebra's own manual text; set
 * unconditionally below as a defensive default rather than a confirmed
 * requirement. See ZEBRA_NOTES.md. */
#define ZEBRA_DOA_CLR_RW_DONE   0x8000u

/* DOA per-drive "clear attention flag" bits (bits 1-4, DRV0-DRV3;
 * p. A-1: "1 CLEARS ATTENTION FLAG"). */
#define ZEBRA_DOA_CLR_ATTN_DRV0 0x4000u
#define ZEBRA_DOA_CLR_ATTN_DRV1 0x2000u
#define ZEBRA_DOA_CLR_ATTN_DRV2 0x1000u
#define ZEBRA_DOA_CLR_ATTN_DRV3 0x0800u

/* --- DIA (Read Data Transfer Status, non-alt mode) bit masks. Bits
 * 6-15 (the fault bits) are numerically confirmed against the
 * Technical Manual's independent "Error Conditions" table (Appendix A
 * p. A-2), whose own "INPUT INSTRUCTION/AC BIT" column names each
 * fault's exact DIA/DIB bit number directly (e.g. "PARITY ERROR / DIA
 * 6", "READ/WRITE FAULT / DIA 15") -- see ZEBRA_NOTES.md for that
 * table transcribed in full; it agrees exactly with the separate
 * accumulator-format block diagram on p. A-1, a real cross-check
 * between two independently-typeset parts of the same manual, not a
 * single source taken on faith. Bit 1 ("CONTROL FULL") and bits 2-5
 * (per-drive attention-request flags) come from the p. A-1 block
 * diagram only -- not independently bit-numbered in the fault table,
 * since they are not fault conditions. Bit 1's "CONTROL FULL" name
 * itself is inferred: the compact Rev. 02 diagram labels it merely
 * "R/W", but the older, fuller Rev. 01 Chapter I summary page draws
 * this exact same bit position's callout line all the way out to a
 * label reading "CONTROL FULL" -- matching the Programming Flowcharts'
 * own "CONTROL FULL?" decision box, and Chapter I's own prose ("A
 * controller can store a command for each channel and issue it when
 * the channel opens"). See ZEBRA_NOTES.md. --- */
#define ZEBRA_DIA_CONTROL_FULL      0x4000u /* bit 1 */
#define ZEBRA_DIA_ATTN_DRV0         0x2000u /* bit 2 */
#define ZEBRA_DIA_ATTN_DRV1         0x1000u /* bit 3 */
#define ZEBRA_DIA_ATTN_DRV2         0x0800u /* bit 4 */
#define ZEBRA_DIA_ATTN_DRV3         0x0400u /* bit 5 */
#define ZEBRA_DIA_PARITY_ERROR      0x0200u /* bit 6 */
#define ZEBRA_DIA_INVALID_SECTOR    0x0100u /* bit 7 */
#define ZEBRA_DIA_ECC_ERROR         0x0080u /* bit 8 */
#define ZEBRA_DIA_BAD_SECTOR        0x0040u /* bit 9 */
#define ZEBRA_DIA_CYLINDER_ERROR    0x0020u /* bit 10 */
#define ZEBRA_DIA_SECTOR_HEAD_ERROR 0x0010u /* bit 11 */
#define ZEBRA_DIA_VERIFY_ERROR      0x0008u /* bit 12 */
#define ZEBRA_DIA_RW_TIMEOUT        0x0004u /* bit 13 */
#define ZEBRA_DIA_DATA_LATE         0x0002u /* bit 14 */
#define ZEBRA_DIA_RW_FAULT          0x0001u /* bit 15 */
#define ZEBRA_DIA_ALLERR            0x03FFu /* bits 6-15 */

/* --- DIB (Read Drive Status, non-alt mode) bit masks -- same p. A-2
 * cross-check for bits 8-15; bits 0-6 from the p. A-1 block diagram
 * only. Bit 7 is shown cross-hatched (reserved/unused) in that
 * diagram, confirmed by the fault table simply having no bit-7 entry
 * either. --- */
#define ZEBRA_DIB_INVALID_STATUS    0x8000u /* bit 0 */
#define ZEBRA_DIB_DRIVE_RESERVED    0x4000u /* bit 1 */
#define ZEBRA_DIB_TRESPASSED        0x2000u /* bit 2 */
#define ZEBRA_DIB_READY             0x1000u /* bit 3 */
#define ZEBRA_DIB_BUSY              0x0800u /* bit 4 */
#define ZEBRA_DIB_POSITIONER_OFFSET 0x0400u /* bit 5 */
#define ZEBRA_DIB_WRITE_DISABLED    0x0200u /* bit 6 */
#define ZEBRA_DIB_INVALID_ADDRESS   0x0080u /* bit 8 */
#define ZEBRA_DIB_ILLEGAL_COMMAND   0x0040u /* bit 9 */
#define ZEBRA_DIB_POWER_FAULT       0x0020u /* bit 10 */
#define ZEBRA_DIB_PACK_UNSAFE       0x0010u /* bit 11 */
#define ZEBRA_DIB_POSITIONER_FAULT  0x0008u /* bit 12 */
#define ZEBRA_DIB_CLOCK_FAULT       0x0004u /* bit 13 */
#define ZEBRA_DIB_WRITE_FAULT       0x0002u /* bit 14 */
#define ZEBRA_DIB_DRIVE_FAULT       0x0001u /* bit 15 */
#define ZEBRA_DIB_ANY_FAULT         0x00FFu /* bits 8-15 */

/* Arbitrary bounded retry count for the post-SEEK attention-flag poll
 * below -- no interrupt-driven completion and no RTC-based real
 * timeout exist in this project yet (same limitation STORAGE_NOTES.md
 * already flagged as unexercised for DSK). examples/vulcan.c avoids
 * needing this at all, by relying on its own manual's documented
 * implicit controller-side deferral of the read/write command until a
 * prior seek finishes -- see zebra.h's header comment on why this
 * driver takes the more conservative, explicitly-polled path instead. */
#define ZEBRA_SEEK_POLL_LIMIT 100000ul /* must be unsigned long: this
                                        * backend's plain `unsigned int`
                                        * is 16-bit (max 65535) and a
                                        * `tries < 100000u` loop with a
                                        * 16-bit `tries` never
                                        * terminates -- confirmed by
                                        * eclipse-cc itself, which warns
                                        * "comparison ... is always
                                        * true" for exactly that mistake
                                        * (caught and fixed while
                                        * writing this file -- see
                                        * ZEBRA_NOTES.md). */

/* --- Bare (unpulsed) register loads and readbacks. --- */
static void dskp_doa(unsigned int word) {
    asm volatile("DOA %0,027" :: "r"(word));
}
static void dskp_doc(unsigned int word) {
    asm volatile("DOC %0,027" :: "r"(word));
}
/* Bare DIA/DIB -- does not clear status, matching disk_probe.s's own
 * DIA usage: a clearing read would be wrong here, since several checks
 * below re-read the same not-yet-cleared status more than once. */
static unsigned int dskp_dia(void) {
    unsigned int r;
    asm volatile("DIA %0,027" : "=r"(r));
    return r;
}
static unsigned int dskp_dib(void) {
    unsigned int r;
    asm volatile("DIB %0,027" : "=r"(r));
    return r;
}

/* Specify Cylinder + P pulse, combined -- starts the SEEK (Programming
 * Flowchart, "Drive Command", p. A-3: "ISSUE A DOCP TO SPECIFY THE
 * CYLINDER AND INITIATE EXECUTION"). Per the Chapter I "S, C AND P
 * FUNCTIONS" note (repeated as "IOPULSE" in Appendix A p. A-1's own
 * summary): "f=P ... Starts the following operations: SEEK,
 * RECALIBRATE, OFFSET, STOP, WRITE DISABLE, RELEASE, and TRESPASS.
 * (Does not affect the Busy flag or Done flag.)" -- so, unlike
 * dskp_dobs_start() below, nothing here can be polled via SKPDN/
 * SKPBN; completion is instead signaled per-drive via the DIA
 * attention bits, polled explicitly by zebra_seek() below. */
static void dskp_docp_seek(unsigned int cylinder) {
    asm volatile("DOCP %0,027" :: "r"(cylinder & 0x03FFu));
}

/* Specify Memory Address + S pulse, combined, then poll for Done.
 * Structurally identical to examples/vulcan.c's dskp_dobs_start() and
 * to eclipse_io.h's outb() macro (DOBS + SKPDN wait loop). Per the
 * same S/C/P note: "f=S Sets the Busy flag to 1; sets the Done flag to
 * 0. Starts the following operations: READ, WRITE, FORMAT, READ
 * BUFFERS, and VERIFY" -- Done genuinely is pollable here, unlike the
 * SEEK pulse above, and per Appendix A p. A-1's own definition ("DONE
 * = READ/WRITE ATTENTION") this is the same flag disk_probe.s already
 * proved SKPDN can poll for, on DSK. */
static void dskp_dobs_start(unsigned int mem_addr_word) {
    asm volatile(
        "DOBS %0,027\n\t"
        "zebra_rw_wait%=:\n\t"
        "SKPDN 027\n\t"
        "JMP zebra_rw_wait%=\n\t"
        :: "r"(mem_addr_word));
}

/* Select `unit` with a NO OPERATION command and read back its DIB.
 * NOP is used rather than skipping the DOA entirely because DIB
 * appears to report on whichever drive the most recent DOA selected
 * (there is no separate "just select, don't reserve" instruction in
 * the manual -- DOA's own caption is "RESERVES A PREVIOUSLY UNRESERVED
 * DRIVE", unconditionally); NOP is the one command code documented to
 * have no other effect. In a single-processor configuration (this
 * driver's whole documented scope -- see zebra.h) reserving a
 * previously-unreserved drive purely as a side effect of a status
 * query is harmless: there is no second controller to contend with. */
unsigned int zebra_status(unsigned int unit) {
    unsigned int doa_word = ZEBRA_DOA_CLR_RW_DONE
                           | ((unsigned int)ZEBRA_CMD_NOP << 7)
                           | ((unit & 3u) << 5);
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
 * drive's attention flag (Programming Flowchart, "Drive Command" flow,
 * p. A-3: DIA attention check -> DOA select+SEEK -> DIB ready check ->
 * DOCP -> [this driver's own simplification: explicitly wait for
 * drive attention here, rather than taking the flowchart's
 * "FOLLOWUP R/W COMMAND?" shortcut straight into the Read/Write flow's
 * own "CONTROL BUSY?"/"CONTROL FULL?" polling -- Appendix A is
 * diagrams only, with no prose anywhere to confirm exactly what those
 * two flags individually gate, so this skeleton takes the
 * conservative, unambiguous path instead: wait for THIS drive's own
 * documented per-drive attention bit, explicitly, before doing
 * anything else. See ZEBRA_NOTES.md]). */
static int zebra_seek(unsigned int unit, unsigned int cylinder) {
    unsigned int attn = zebra_attn_bit(unit);
    unsigned int doa_word, dib_status, dia_status;
    unsigned long tries;

    doa_word = ZEBRA_DOA_CLR_RW_DONE
             | ((unsigned int)ZEBRA_CMD_SEEK << 7)
             | ((unit & 3u) << 5);
    dskp_doa(doa_word);

    dib_status = dskp_dib();
    if (!(dib_status & ZEBRA_DIB_READY))
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

    /* Clear the attention flag we just consumed (DOA's own "1 clears
     * attention flag" semantics, p. A-1) so the next command doesn't
     * see a stale, already-serviced event. */
    dskp_doa(zebra_clr_attn_bit(unit));

    dib_status = dskp_dib();
    if (!(dib_status & ZEBRA_DIB_READY))
        return -3;
    if (dib_status & ZEBRA_DIB_ANY_FAULT)
        return -4;
    return 0;
}

/* Shared core for both directions. is_write: 0 = read, nonzero =
 * write. Single sector only (surface/sector addressing, count = two's
 * complement of 1). */
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

    /* Flat sector index -> cylinder/surface/sector. This driver's own
     * invention -- the Technical Manual only defines cylinder/surface/
     * sector addressing, never a flat block number (unlike DSK, which
     * genuinely is flat in hardware; see STORAGE_NOTES.md). Sector is
     * the fastest-varying component, then surface, then cylinder,
     * matching examples/vulcan.c's own equivalent choice for the same
     * reason: no manual guidance either way, so this picks the
     * conventional CHS minor-to-major order. */
    sector = (unsigned int)(blockno % ZEBRA_SECTORS_PER_TRACK);
    tmp = blockno / ZEBRA_SECTORS_PER_TRACK;
    surface = (unsigned int)(tmp % ZEBRA_SURFACES_PER_UNIT);
    cylinder = (unsigned int)(tmp / ZEBRA_SURFACES_PER_UNIT);

    rc = zebra_seek(unit, cylinder);
    if (rc != 0)
        return rc;

    /* Select the drive again and store the READ/WRITE command. DOC's
     * meaning is context-sensitive on the *previous* DOA (p. A-1: "IF
     * PREVIOUS DOA SPECIFIED SEEK" vs. "IF PREVIOUS DOA DID NOT
     * SPECIFY SEEK"), so this DOA -- with a non-SEEK command -- must
     * be issued before the surface/sector/count DOC below, to put DOC
     * back into its surface/sector/count meaning. */
    doa_word = ZEBRA_DOA_CLR_RW_DONE
             | ((unsigned int)(is_write ? ZEBRA_CMD_WRITE : ZEBRA_CMD_READ) << 7)
             | ((unit & 3u) << 5);
    dskp_doa(doa_word);

    dib_status = dskp_dib();
    if (!(dib_status & ZEBRA_DIB_READY))
        return -3;

    /* Specify Surface, Sector and Count (p. A-1): bit 0 = MAP MODE
     * (left 0 -- direct physical addressing, no map-select attempted
     * here), bits 1-5 = surface (0-18), bits 6-10 = starting sector
     * (0-23), bits 11-15 = sector count as two's complement (all-ones
     * = -1 = transfer exactly one sector). Both surface and sector fit
     * their 5-bit fields directly (19 and 24 both < 32) -- unlike
     * vulcan.c's DSKP, which needs an extra "Specify Extended Sector
     * and Count" DOC first, because its 35 sectors/track need a 6th
     * bit (see zebra.h's header comment on this difference between
     * the two devices). */
    count5 = (unsigned int)(-1) & 0x1Fu; /* two's complement of 1, 5 bits */
    doc_word = ((surface & 0x1Fu) << 10)
             | ((sector  & 0x1Fu) << 5)
             | count5;
    dskp_doc(doc_word);

    /* Specify Memory Address (p. A-1): bit 0 = extended memory address
     * LSB (left 0 -- buf must be a normal, non-extended pointer,
     * 0-32767, same restriction as mmpu.h's Phase 1 API and
     * vulcan.h). Combined with the S pulse that starts the transfer;
     * dskp_dobs_start() polls SKPDN to completion. */
    dob_word = (unsigned int)(unsigned long)buf & 0x7FFFu;
    dskp_dobs_start(dob_word);

    /* Check status. DIA's error-bits field (bits 6-15) is returned
     * directly to the caller on a nonzero read, rather than collapsed
     * to a single bit, so a caller with ZEBRA_NOTES.md open can tell
     * which fault(s) actually happened -- see zebra.h's return-value
     * comment. */
    dia_status = dskp_dia();
    dib_status = dskp_dib();
    if (dib_status & ZEBRA_DIB_ANY_FAULT)
        return -4;
    if (dia_status & ZEBRA_DIA_ALLERR)
        return (int)(dia_status & ZEBRA_DIA_ALLERR);

    return 0;
}

int zebra_read_block(unsigned int unit, unsigned long blockno, unsigned int *buf) {
    return zebra_rw(unit, blockno, buf, 0);
}

int zebra_write_block(unsigned int unit, unsigned long blockno, unsigned int *buf) {
    return zebra_rw(unit, blockno, buf, 1);
}
