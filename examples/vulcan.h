#ifndef _VULCAN_H
#define _VULCAN_H

/* Driver skeleton for the DG Model 6122 "Vulcan" DG-Disc Storage
 * Subsystem (device mnemonic DSKP in the manual). See VULCAN_NOTES.md
 * at the repo root for the full register/command/status citation
 * trail against the real Programmer's Reference (014-000644-00, Rev.
 * 00, First Printing, December 1979) and the Maintenance Manual
 * (015-000107-00, 1980).
 *
 * *** UNVERIFIED ON A SIMULATOR OR REAL HARDWARE ***
 * This project's SIMH build (~/dev/simh-src/BIN/eclipse) does not
 * model this controller at all -- confirmed: no "vulcan"/"6122"/"dskp"
 * reference anywhere in the NOVA/ source directory's .c files. Unlike
 * every other
 * driver in this directory (disk_probe.s/DSK, mmpu.c/MAP, fps.c/
 * FPS100 -- all single-stepped against a real or simulated device),
 * this code has never executed against real Vulcan hardware or any
 * simulation of it. It is a from-the-manual skeleton, built by
 * transcribing the Programmer's Reference's own register/bit tables
 * and worked programming sequence (its "Phase I-V" walkthrough,
 * pgmref.pdf pages 11-12) as literally as possible. It compiles
 * against this project's real eclipse-cc toolchain (see
 * VULCAN_NOTES.md's "What was and wasn't checked" section for exactly
 * what that does and does not prove) but the register bit-math has
 * only been hand-traced against the manual's tables, not executed.
 * Treat every claim here as "this is what the manual says to do,"
 * not "this is confirmed to work."
 *
 * Real hardware identity: DG Model 6122 Series DG/Disc Storage
 * Subsystem. Mnemonic DSKP. Device select code 027 octal (the manual
 * also lists an alternate select code 067 octal -- see VULCAN_NOTES.md
 * for what that means and why it's not used here). Up to 4 drives per
 * controller (unit 0-3). Each drive: 815 cylinders (0-1456 octal) x
 * 19 surfaces (0-22 octal) x 35 sectors (0-42 octal) x 512 bytes
 * (256 words)/sector = 277,491,200 bytes/drive -- this exact number
 * is printed in the manual's own Introduction (pgmref.pdf p.3) and is
 * reproduced here by multiplying the geometry figures on the same
 * page, as an internal consistency check (see VULCAN_NOTES.md).
 *
 * Scope: raw single-sector block read/write only, single-processor
 * configuration (per the manual's own guidance to ignore Release/
 * Trespass/Reserved/Invalid-Status handling outside a dual-processor
 * setup -- pgmref.pdf p.11), polled completion (no interrupt-driven
 * completion), no ECC error-correction attempt on a detected data
 * error (the fault is reported, not recovered), no multi-sector
 * transfers, and no BMC "mapped" (logical) addressing -- only direct
 * physical memory addresses within this backend's normal 0-32767
 * pointer range, the same restriction mmpu.h's Phase 1 API and
 * examples/disk_probe.s already operate under.
 */

#define DSKP_SURFACES_PER_CYL 19u
#define DSKP_SECTORS_PER_SURF 35u
#define DSKP_WORDS_PER_SECTOR 256u
/* 815 * 19 * 35 = 541,975 sectors/drive -- cross-checked against the
 * manual's own byte total: 541,975 * 512 = 277,491,200 bytes, exactly
 * matching pgmref.pdf p.3's "Bytes drive 277,491,200" figure. See
 * VULCAN_NOTES.md for this arithmetic written out. */
#define DSKP_SECTORS_PER_DRIVE 541975UL

/* vulcan_read_block / vulcan_write_block: transfer exactly one
 * 256-word sector between drive `unit` (0-3) and `buf` (must point to
 * a 256-word buffer within this backend's normal addressable range --
 * no extended/BMC-mapped addressing is used). `blockno` is a flat
 * sector index (0 to DSKP_SECTORS_PER_DRIVE-1); this function converts
 * it to cylinder/surface/sector internally (see vulcan.c).
 *
 * Return value (a coarse status code, not a decoded fault -- see
 * vulcan.c's comments on why finer decoding was left out of this
 * skeleton):
 *   0  = success (no fault bits set in either status register read
 *        back after the transfer).
 *  -1  = bad argument (unit > 3, or blockno out of range).
 *  -2  = drive not ready after the Phase I drive-status check
 *        (pgmref.pdf p.9's DIB "Ready" bit, bit 3, was 0).
 *  -3  = a fault bit was set in the post-transfer status read (either
 *        DIA's bit 15 "R/W fault" or DIB's bit 15 "Drive fault" --
 *        pgmref.pdf pp.9-10). The caller gets no more detail than
 *        that from this skeleton; a real driver would want to return
 *        the full DIA/DIB words for the caller (or a recovery
 *        routine) to decode against VULCAN_NOTES.md's fault tables.
 */
int vulcan_read_block(unsigned int unit, unsigned long blockno, unsigned int *buf);
int vulcan_write_block(unsigned int unit, unsigned long blockno, unsigned int *buf);

#endif
