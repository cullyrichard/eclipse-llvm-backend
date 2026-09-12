#include "vulcan.h"

/* See vulcan.h's header comment before trusting any of this against
 * real Vulcan hardware -- this has never been executed, only compiled.
 *
 * Register-load instructions (DOA/DOB/DOC, bare, no pulse) all have a
 * genuine per-instruction accumulator operand -- unlike the MMPU's
 * LMP instruction (see mmpu.c's header comment on why *that* code
 * routes everything through named globals instead), so this file uses
 * ordinary "r"-constrained inline-asm operands throughout, the same
 * pattern examples/fps.h's fpu_out/fpu_in already use for the FPS100's
 * DOA/DOB pair. The device select code (027 octal) is pasted directly
 * as literal text in each asm string, exactly as examples/fps.h does
 * for FPS100's device 054 -- no `dev DSKP = 027` declaration is used,
 * to sidestep any question of whether dgasm accepts a symbolic device
 * name inside a %-operand-substituted instruction (not needed either
 * way: a bare octal literal in the mnemonic's device-code slot is
 * definitely accepted -- every existing driver in this project does
 * exactly that for at least one instruction).
 *
 * Pulse discipline (why bare DOA/DOC are used for register loads, and
 * a pulse is combined with only the *last* register-setting
 * instruction of each phase): the Programmer's Reference's S/C/P
 * device-flag commands are documented as acting on the *device*, not
 * on whichever specific A/B/C-channel instruction happened to carry
 * the pulse (pgmref.pdf p.4, "f=S / f=C / f=P" descriptions apply to
 * "the controller" as a whole, not to "this DOA" specifically) --
 * the same behavior examples/disk_probe.s and STORAGE_NOTES.md found
 * (by reading nova_dsk.c directly) for the DSK device's simulated
 * hardware. Since there is no simulator model to confirm this
 * empirically for DSKP, this file follows the manual's own literal
 * phrasing instead: "issue a Specify Cylinder instruction (DOC) plus
 * a P device flag command" (Phase II, pgmref.pdf p.12) and "issue a
 * Specify Memory Address instruction (DOB) plus an S device flag
 * command" (Phase IV, same page) are each implemented as ONE combined
 * register-load+pulse instruction (DOCP / DOBS) -- dgasm's grammar
 * for this is already proven in eclipse_io.h's outa/outb/outc macros,
 * which use the identical DOAS/DOBS/DOCS combined forms for other
 * devices. Every *other* register load in this file (the Phase I/III
 * Specify Command DOA, and Phase IV's two Specify Sector/Count DOCs)
 * is issued bare, unpulsed, exactly mirroring disk_probe.s's own
 * stated reason for doing so.
 */

/* --- Command codes: DOA bits 5-8 (pgmref.pdf p.5, "Specify Command,
 * Drive and Extended Address" bit table). --- */
#define DSKP_CMD_READ          000
#define DSKP_CMD_RECAL         001
#define DSKP_CMD_SEEK          002
#define DSKP_CMD_STOP          003
#define DSKP_CMD_OFFSET_FWD    004
#define DSKP_CMD_OFFSET_REV    005
#define DSKP_CMD_WRITE_DISABLE 006
#define DSKP_CMD_RELEASE       007
#define DSKP_CMD_TRESPASS      010
#define DSKP_CMD_ALT_MODE1     011
#define DSKP_CMD_ALT_MODE2     012
#define DSKP_CMD_NOP           013
#define DSKP_CMD_VERIFY        014
#define DSKP_CMD_READ_BUFFERS  015
#define DSKP_CMD_WRITE         016
#define DSKP_CMD_FORMAT        017

/* --- Status bit masks, DG bit-numbering (bit 0 = MSB = 0x8000, bit 15
 * = LSB = 0x0001) matching the manual's own convention throughout. --- */
#define DSKP_DIA_CONTROL_FULL  0x8000u /* DIA bit 0 (Read Data Transfer
                                         * Status, pgmref.pdf p.9) */
#define DSKP_DIA_RWFAULT       0x0001u /* DIA bit 15, "any of the above
                                         * faults or a drive fault" */
#define DSKP_DIB_READY         0x1000u /* DIB bit 3 (Read Drive Status,
                                         * pgmref.pdf p.9) */
#define DSKP_DIB_DRIVE_FAULT   0x0001u /* DIB bit 15 */

/* --- Bare (unpulsed) register loads --- */
static void dskp_doa(unsigned int word) {
    asm volatile("DOA %0,027" :: "r"(word));
}
static void dskp_doc(unsigned int word) {
    asm volatile("DOC %0,027" :: "r"(word));
}

/* --- Readbacks (bare DIA/DIB -- does not clear status, matching
 * disk_probe.s's own DIA usage and its stated reason: a clearing read
 * would be wrong here since Phase I/III re-check control full using
 * the *same* not-yet-cleared status). --- */
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

/* --- Phase II: Specify Cylinder + P pulse, combined. Per pgmref.pdf
 * p.4's S/C/P table, "f=P ... does not affect the Busy flag or Done
 * flag" -- so, unlike the Phase IV trigger below, there is nothing to
 * poll here. Per Phase IV's own text (pgmref.pdf p.12): "When the
 * selected drive completes the previous seek operation ... the
 * controller transmits the stored read/write command to the adapter
 * and the operation begins" -- i.e. the *controller* defers the
 * read/write until the seek it already knows about finishes; software
 * does not need to poll for seek completion before proceeding to
 * Phase III/IV. This driver relies on that documented behavior instead
 * of polling DIB's Busy bit itself. --- */
static void dskp_docp_seek(unsigned int cylinder) {
    asm volatile("DOCP %0,027" :: "r"(cylinder & 0x3FFu));
}

/* --- Phase IV: Specify Memory Address + S pulse, combined, then poll
 * for Done -- structurally identical to eclipse_io.h's outb() macro
 * (DOBS + SKPDN wait loop), just not routed through that macro so this
 * file doesn't depend on IO_CHECK_DEVICE_'s stringize-of-an-octal-
 * literal working for a symbol that isn't spelled with a leading zero
 * inside this expression (027 already has one, so outb(027, word)
 * would in fact work identically -- this is written out by hand
 * mainly so the Phase-specific comment above stays attached to the
 * code it explains). --- */
static void dskp_dobs_start(unsigned int mem_addr_word) {
    asm volatile(
        "DOBS %0,027\n\t"
        "vulcan_rw_wait%=:\n\t"
        "SKPDN 027\n\t"
        "JMP vulcan_rw_wait%=\n\t"
        :: "r"(mem_addr_word));
}

/* Shared core for both directions. is_write: 0 = read, nonzero = write. */
static int vulcan_rw(unsigned int unit, unsigned long blockno,
                      unsigned int *buf, int is_write) {
    unsigned long cyl_and_surf;
    unsigned int cyl, surface, sector;
    unsigned int sector6, count6;
    unsigned int doa_word, doc1_word, doc2_word, dob_word;
    unsigned int dia_status, dib_status;

    if (unit > 3u || blockno >= DSKP_SECTORS_PER_DRIVE)
        return -1;

    /* Flat sector index -> cylinder/surface/sector. Sector is the
     * fastest-varying component, then surface, then cylinder -- this
     * matches the manual's own auto-increment order (pgmref.pdf p.4,
     * "Programming Details": "the sector address and sector count
     * registers increment halfway through a sector data transfer, and
     * the surface address register increments following a data
     * transfer to or from the last sector on a track"). */
    sector = (unsigned int)(blockno % DSKP_SECTORS_PER_SURF);
    cyl_and_surf = blockno / DSKP_SECTORS_PER_SURF;
    surface = (unsigned int)(cyl_and_surf % DSKP_SURFACES_PER_CYL);
    cyl = (unsigned int)(cyl_and_surf / DSKP_SURFACES_PER_CYL);

    /* --- Phase I: select a drive and specify a seek command
     * (pgmref.pdf p.11). Single-processor simplification per the
     * manual's own note ("ignore the release and trespass commands
     * and also the trespassed flag, the drive reserved flag and the
     * invalid status flag") -- only the Ready bit is checked below. */
    while (dskp_dia() & DSKP_DIA_CONTROL_FULL)
        ;
    doa_word = (1u << 15)                 /* bit0: Clear R/W Done */
             | ((unsigned int)DSKP_CMD_SEEK << 7)
             | ((unit & 3u) << 5);
    dskp_doa(doa_word);

    dib_status = dskp_dib();
    if (!(dib_status & DSKP_DIB_READY))
        return -2;

    /* --- Phase II: position the heads. --- */
    dskp_docp_seek(cyl);

    /* --- Phase III: select a drive and specify a read/write command
     * (pgmref.pdf p.12). --- */
    while (dskp_dia() & DSKP_DIA_CONTROL_FULL)
        ;
    doa_word = (1u << 15)
             | ((unsigned int)(is_write ? DSKP_CMD_WRITE : DSKP_CMD_READ) << 7)
             | ((unit & 3u) << 5);
    dskp_doa(doa_word);

    /* --- Phase IV: specify sector/surface/count, then memory address
     * + S pulse to start the transfer (pgmref.pdf p.12). One sector
     * only: starting sector = `sector` (not two's complement -- only
     * the *count* field is two's complement, pgmref.pdf p.6-7), count
     * = two's complement of 1, i.e. all-ones in the 6-bit field. */
    sector6 = sector & 0x3Fu;
    count6 = (unsigned int)(-1) & 0x3Fu; /* two's complement of 1 */

    /* First DOC ("Specify Extended Sector and Count", pgmref.pdf p.6):
     * bit5 = sector address MSB, bit10 = sector count MSB. Must
     * precede the second DOC per the manual's own NOTE on that page. */
    doc1_word = (((sector6 >> 5) & 1u) << 10)
              | (((count6  >> 5) & 1u) << 5);
    dskp_doc(doc1_word);

    /* Second DOC ("Specify Surface, Sector and Count", pgmref.pdf p.6):
     * bit0 = MAP (left 0 -- direct physical addressing, no BMC-mapped
     * addressing attempted here), bits1-5 = surface, bits6-10 = sector
     * low 5 bits, bits11-15 = count low 5 bits. */
    doc2_word = ((surface & 0x1Fu) << 10)
              | ((sector6  & 0x1Fu) << 5)
              | (count6 & 0x1Fu);
    dskp_doc(doc2_word);

    /* DOB ("Specify Memory Address", pgmref.pdf p.7): bit0 = extended
     * memory address LSB (left 0 -- buf must be a normal, non-extended
     * pointer, 0-32767, same restriction as mmpu.h's Phase 1 API),
     * bits1-15 = memory address. Combined with the S pulse that starts
     * the transfer; dskp_dobs_start() polls SKPDN to completion. */
    dob_word = (unsigned int)(unsigned long)buf & 0x7FFFu;
    dskp_dobs_start(dob_word);

    /* --- Check status. This is a coarse OR of each register's single
     * summary fault bit -- see vulcan.h's return-value comment for why
     * a real driver would want more than that. --- */
    dia_status = dskp_dia();
    dib_status = dskp_dib();
    if ((dia_status & DSKP_DIA_RWFAULT) || (dib_status & DSKP_DIB_DRIVE_FAULT))
        return -3;

    return 0;
}

int vulcan_read_block(unsigned int unit, unsigned long blockno, unsigned int *buf) {
    return vulcan_rw(unit, blockno, buf, 0);
}

int vulcan_write_block(unsigned int unit, unsigned long blockno, unsigned int *buf) {
    return vulcan_rw(unit, blockno, buf, 1);
}
