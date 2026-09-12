#ifndef _KISMET_H
#define _KISMET_H

/* kismet.h -- C-callable block driver for the DG-Disk Storage Subsystem
 * ("Kismet"), Models 6160/6161/6214, device mnemonic DSKP.
 *
 * See KISMET_NOTES.md at the repo root for the full register-layout
 * writeup, page-by-page manual citations, and an honest list of what
 * real-hardware testing would still be needed before trusting this.
 * Short version: this is a driver *skeleton*. It compiles and (per
 * kismet_probe.s) assembles against the real dgasm mnemonics the manual
 * documents, but it has NEVER been executed against a real DSKP
 * controller or a simulator that models one -- this project's SIMH
 * build (`~/dev/simh-src/BIN/eclipse`) does not implement Kismet at
 * all (confirmed: no "kismet"/"6160"/"6161"/"6214" reference anywhere
 * in the NOVA source directory's C files), unlike the DSK/DKP devices
 * STORAGE_NOTES.md's disk_probe.s could single-step-verify.
 *
 * Real device select code: DSKP, device code 027 octal (Programmer's
 * Reference rev 1, p.4, "Programming Summary": "Device code 27(Alt.
 * 67)"). The manual documents a jumper-selectable alternate code 067
 * octal for systems where 027 is already in use by something else --
 * this driver hard-codes the primary 027; see KISMET_NOTES.md if a
 * real system needs the alternate.
 *
 * As of this session, kismet.c is implemented on top of a shared DSKP-
 * family register core (examples/dskp_common.h/.c) also used by
 * examples/zebra.c and examples/vulcan.c -- the three drivers were
 * unified after all three independently discovered they share the
 * identical device select code and DSKP register convention, and after
 * this file's own KISMET_NOTES.md documented a real, reproduced build
 * conflict between Kismet's and Vulcan's `dev DSKP = 027` declarations
 * (see DSKP_FAMILY_NOTES.md at the repo root for the unification
 * design and confirmation that this conflict is actually fixed). This
 * header (the public kismet_read_block/kismet_write_block API and
 * Kismet's own geometry constants) is unchanged in shape by that
 * refactor -- only kismet.c's internals moved onto the shared core,
 * including switching its inline-asm register discipline from
 * global+ELDA/ESTA to the same "r"-constrained operand style
 * zebra.c/vulcan.c already used (see kismet.c's own header comment on
 * why that switch is safe here).
 */

/* Which physical drive (0 or 1) -- DSKP supports at most 2 drives per
 * controller (Programmer's Reference p.5, DOA bits 9-10: "Drive
 * (0-1)"), unlike DKP's 4 units. */
#define KISMET_UNIT_0 0
#define KISMET_UNIT_1 1

/* Real drive geometry, Programmer's Reference rev 1 p.3's own table
 * (cross-checked against the Maintenance Guide, 015-000910-01 p.1-1:
 * "Each data surface has two heads, 823 tracks and 35 sectors per
 * track" -- independently confirms the 823-cylinder/35-sector/track
 * figures below for the 73/147 MB drives). Sectors/track is the same
 * (35 decimal = 042 octal, i.e. sectors numbered 0-34 decimal / 0-42
 * octal) across all three drive sizes -- the only things that vary are
 * head count and cylinder count. */
#define KISMET_SECTORS_PER_TRACK 35 /* 0-42 octal, all models */

#define KISMET_HEADS_6160 5   /* 73 MB drive, heads 0-4 (0-4 octal) */
#define KISMET_CYLS_6160 823  /* 0-822 decimal = 0-1466 octal */

#define KISMET_HEADS_6161 10  /* 147 MB drive, heads 0-9 (0-11 octal) */
#define KISMET_CYLS_6161 823

#define KISMET_HEADS_6214 40  /* 600 MB drive, heads 0-39 (0-47 octal) */
#define KISMET_CYLS_6214 843  /* 0-842 decimal = 0-1512 octal */

/* Words per sector: 512 bytes/sector (Programmer's Reference p.3) / 2
 * bytes per Eclipse word = 256 words. Same for all three drive sizes. */
#define KISMET_WORDS_PER_SECTOR 256

/* DIA (Read Data Transfer Status) error/status bit masks -- default
 * context, Programmer's Reference rev 1 p.8. DG bit numbering is
 * MSB-first (bit 0 = leftmost / value 0100000 octal, bit 15 = LSB /
 * value 1); mask = 1 << (15 - bit). */
#define KISMET_DIA_CTRL_FULL  0100000 /* bit 0: previous stored command not yet issued */
#define KISMET_DIA_RW_DONE    0040000 /* bit 1: read/write operation terminated */
#define KISMET_DIA_DRV0_DONE  0020000 /* bit 2 */
#define KISMET_DIA_DRV1_DONE  0010000 /* bit 3 */
#define KISMET_DIA_PARITY     0001000 /* bit 6 */
#define KISMET_DIA_ILL_SECTOR 0000400 /* bit 7 */
#define KISMET_DIA_ECC        0000200 /* bit 8 */
#define KISMET_DIA_BAD_SECTOR 0000100 /* bit 9 */
#define KISMET_DIA_CYL_ERR    0000040 /* bit 10 */
#define KISMET_DIA_HDSECT_ERR 0000020 /* bit 11 */
#define KISMET_DIA_VERIFY_ERR 0000010 /* bit 12 */
#define KISMET_DIA_RW_TIMEOUT 0000004 /* bit 13 */
#define KISMET_DIA_DATA_LATE  0000002 /* bit 14 */
#define KISMET_DIA_RW_FAULT   0000001 /* bit 15: OR of all of the above,
                                        * or a drive fault on the
                                        * currently selected drive */
#define KISMET_DIA_ALLERR (KISMET_DIA_PARITY | KISMET_DIA_ILL_SECTOR | \
    KISMET_DIA_ECC | KISMET_DIA_BAD_SECTOR | KISMET_DIA_CYL_ERR | \
    KISMET_DIA_HDSECT_ERR | KISMET_DIA_VERIFY_ERR | \
    KISMET_DIA_RW_TIMEOUT | KISMET_DIA_DATA_LATE | KISMET_DIA_RW_FAULT)

/* DIB (Read Drive Status) bit masks -- default context, Programmer's
 * Reference rev 1 p.9. */
#define KISMET_DIB_READY     0020000 /* bit 3 */
#define KISMET_DIB_BUSY      0010000 /* bit 4: seek/recal in progress */
#define KISMET_DIB_WR_DIS    0002000 /* bit 6: front-panel write disable */
#define KISMET_DIB_POS_FAULT 0000010 /* bit 12 */
#define KISMET_DIB_DRV_FAULT 0000001 /* bit 15 */

/* kismet_read_block / kismet_write_block: raw block I/O, no filesystem
 * knowledge. `buf` must point to KISMET_WORDS_PER_SECTOR (256) words --
 * a fixed multiple of the 512-byte real sector size, transferred via
 * simulated-DMA-equivalent hardware "data channel" or BMC transfer
 * (Programmer's Reference p.3), no per-word CPU involvement once
 * triggered, same as this project's already-verified DSK driver
 * (STORAGE_NOTES.md).
 *
 * `blockno` is a driver-invented LINEAR block number -- the real
 * DSKP hardware addresses sectors by (cylinder, head, sector), not a
 * flat block number (unlike DSK); this driver divides it out
 * internally using `heads` and KISMET_SECTORS_PER_TRACK
 * (cyl = blockno / (heads*35), head = (blockno/35) % heads,
 * sector = blockno % 35). This mapping is a software convention
 * invented here, not anything the manual specifies -- see
 * KISMET_NOTES.md.
 *
 * Returns 0 on a clean transfer (DIA read back as 0, no error bits
 * set); returns the raw final DIA status word (nonzero, KISMET_DIA_*
 * bits) if the drive accepted the command but the transfer faulted;
 * returns -1 (a sentinel, not a real DIA value -- distinguishable
 * because a real DIA word would need literally every error bit set
 * simultaneously to collide with it) if the initial DIB status check
 * found the drive not Ready or drive-faulted *before* any seek/
 * transfer was attempted at all. `heads` selects which drive geometry
 * to use for the CHS division (pass KISMET_HEADS_6160/6161/6214 for the
 * attached drive model; there is no runtime auto-detect of drive size wired up yet,
 * though DIB alternate mode 1's Drive 0/1 ID bits (p.9) could
 * provide one -- see KISMET_NOTES.md). */
int kismet_read_block(int unit, int heads, unsigned int blockno, void *buf);
int kismet_write_block(int unit, int heads, unsigned int blockno, void *buf);

#endif
