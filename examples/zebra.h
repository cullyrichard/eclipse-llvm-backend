#ifndef _ZEBRA_H
#define _ZEBRA_H

/* Driver skeleton for the DG Model 6060/6061/6067 "Zebra" DG-DISC
 * Storage Subsystem (device mnemonic DSKP in the Technical Manual's own
 * accumulator-format diagrams; the DG-wide *standard* I/O device code
 * table names the same select code DPF -- "DG/Disc storage subsystem").
 * See ZEBRA_NOTES.md at the repo root for the full register/command/
 * status citation trail against the real 6060 Series Technical Manual
 * (015-000061-03 Rev 3, 1976-1980) and the ECLIPSE S/140 Programmer's
 * Reference (014-000642-02 Rev 02, Apr 1981).
 *
 * *** UNVERIFIED ON A SIMULATOR OR REAL HARDWARE ***
 * This project's SIMH build (~/dev/simh-src/BIN/eclipse) does not
 * model this controller at all -- confirmed: no "zebra"/"6060"/"6061"/
 * "6067" reference anywhere in the NOVA/ source directory's .c files.
 * Unlike disk_probe.s/DSK, mmpu.c/MAP, or fps.c/FPS100 (all single-
 * stepped against a real or simulated device), this code has never
 * executed against real Zebra hardware or any simulation of it. It IS
 * confirmed to compile and assemble against this project's real
 * eclipse-cc + dgasm pipeline (see ZEBRA_NOTES.md's "What was and
 * wasn't checked" section for exactly what that does and does not
 * prove) -- but the register bit-math has only been hand-traced
 * against the manual's own diagrams, not executed. Treat every claim
 * here as "this is what the manual says to do," not "this is confirmed
 * to work."
 *
 * Real hardware identity: DG 6060-series DG-DISC Storage Subsystem
 * (6060 single-density, 6061 double-density, 6067 50MB variant --
 * see ZEBRA_NOTES.md on which numbers below are common to all three
 * vs. specific to one). Select code 027 octal (DG-wide standard table:
 * mnemonic DPF, priority mask bit 7, "DG/Disc storage subsystem";
 * alternate select code 067 octal = DPF1, second controller). Up to 4
 * drives per controller (unit 0-3). Single-density (6060) geometry:
 * 411 cylinders x 19 surfaces x 24 sectors x 512 bytes (256 words)/
 * sector = 95,956,992 bytes/drive -- this exact figure and the 411
 * cylinder count are both printed directly in the Technical Manual's
 * own "Programming Summary" / "Instruction Summary" pages and in its
 * Chapter I "Summary of Characteristics" table, and reproduced here as
 * an internal consistency check (surfaces * sectors * bytes-per-sector
 * * cylinders = the manual's own stated capacity -- see ZEBRA_NOTES.md).
 *
 * Interesting cross-check found *within this same working session*:
 * the DG Model 6122 "Vulcan" DG-Disc Storage Subsystem (a later,
 * separate product -- see examples/vulcan.h/vulcan.c, written by a
 * parallel effort scoping that device, NOT modified by this file) uses
 * the *exact same* select code (027 octal, alt 067), the *exact same*
 * DSKP mnemonic, and a byte-for-byte identical 16-command list at the
 * same bit position (DOA bits 5-8) as this file's ZEBRA_CMD_* table
 * below. That is strong, independent corroboration that "DSKP"/select
 * code 027 is Data General's standing, generation-spanning register
 * convention for its whole "DG/Disc Storage Subsystem" product
 * category, not a Zebra-specific coincidence -- Vulcan looks like a
 * higher-density hardware successor to this same programming model
 * (815 cylinders x 19 surfaces, same as Zebra's own double-density 6061
 * geometry, but 35 sectors/track instead of 24, needing one extra
 * "Specify Extended Sector and Count" DOC step Zebra's narrower 5-bit
 * sector field doesn't need -- see ZEBRA_NOTES.md).
 *
 * Scope: raw single-sector block read/write only, single-processor
 * configuration (Release/Trespass/Reserved handling is out of scope,
 * same simplification Vulcan's driver documents for its own manual's
 * equivalent guidance), polled completion (no interrupt-driven
 * completion -- this project has no interrupt-driven scheduler yet,
 * same scope line STORAGE_NOTES.md already drew for DSK), no ECC
 * error-correction attempt on a detected data error (the fault is
 * reported, not recovered), no multi-sector transfers (Zebra can
 * transfer up to 32 contiguous sectors per operation per the Technical
 * Manual's own Chapter I "Summary of Characteristics" -- a natural next
 * increment, not attempted here), and no extended/64K memory
 * addressing -- only direct physical addresses within this backend's
 * normal 0-32767 pointer range, the same restriction mmpu.h's Phase 1
 * API, disk_probe.s, and vulcan.h all already operate under.
 */

#define ZEBRA_SURFACES_PER_UNIT 19u   /* 19 data surfaces (of 20 total --
                                       * the 20th is servo-only and not
                                       * software-addressable). */
#define ZEBRA_SECTORS_PER_TRACK 24u
#define ZEBRA_WORDS_PER_SECTOR  256u  /* 512 bytes/sector, same size as
                                       * DSK's sector (disk_probe.s). */
/* Cylinder count depends on single- vs. double-density platters --
 * 411 (6060) is the conservative default; set to 815 for a 6061
 * (double-density) unit. The 50MB 6067 variant has its own Technical
 * Manual (015-000076-00) not independently cross-checked against this
 * number in this session -- confirm before relying on it for a 6067
 * (see ZEBRA_NOTES.md). */
#define ZEBRA_CYLINDERS 411u

/* 411 * 19 * 24 = 187,416 sectors/drive (single-density) -- cross-
 * checked against the manual's own byte total: 187,416 * 512 =
 * 95,956,992 bytes, exactly matching the Technical Manual's stated
 * "Capacity per Drive / Single Density" figure. See ZEBRA_NOTES.md for
 * this arithmetic written out, and for the 815-cylinder (double-
 * density) equivalent. */
/* Cast the first operand to force the whole product into unsigned-long
 * arithmetic -- 411 * 19 * 24 = 187,416 overflows a 16-bit unsigned
 * int (this backend's plain `unsigned int`) well before the final
 * multiply, and 815 * 19 * 24 = 371,640 overflows even worse. */
#define ZEBRA_SECTORS_PER_DRIVE ((unsigned long)ZEBRA_CYLINDERS * ZEBRA_SURFACES_PER_UNIT * ZEBRA_SECTORS_PER_TRACK)

/* zebra_read_block / zebra_write_block: transfer exactly one 256-word
 * sector between drive `unit` (0-3) and `buf` (must point to a
 * 256-word buffer within this backend's normal addressable range --
 * no extended addressing is used). `blockno` is a flat sector index
 * (0 to ZEBRA_SECTORS_PER_DRIVE-1, given the ZEBRA_CYLINDERS setting
 * above); this driver's own invention, not something the Technical
 * Manual defines -- the hardware only knows cylinder/surface/sector
 * addressing, so zebra.c converts internally (see its own comments).
 *
 * Return value (a coarse status code, not a decoded fault -- matching
 * vulcan.c's own choice and for the same reason: finer decoding would
 * mean committing to bit-for-bit DIA/DIB semantics this session could
 * only hand-trace, not execute):
 *   0  = success (no fault bits set in either status register read
 *        back after the transfer).
 *  -1  = bad argument (unit > 3, or blockno out of range).
 *  -2  = seek never completed (the per-drive "attention" flag this
 *        driver polls for after issuing SEEK, per the Technical
 *        Manual's own Programming Flowcharts, never came back within
 *        ZEBRA_SEEK_POLL_LIMIT iterations).
 *  -3  = drive not Ready (DIB bit 3) after the seek.
 *  -4  = a fault bit was set in DIB (bits 8-15) -- either right after
 *        the seek, or right after the transfer (see zebra.c/
 *        ZEBRA_NOTES.md for the full per-bit table).
 *  >0  = the transfer completed but DIA's error-bits field (bits 6-15,
 *        masked by ZEBRA_DIA_ALLERR) was nonzero after it; the return
 *        value IS that masked field, so the caller can decode it
 *        against ZEBRA_NOTES.md's DIA fault table directly.
 */
int zebra_read_block(unsigned int unit, unsigned long blockno, unsigned int *buf);
int zebra_write_block(unsigned int unit, unsigned long blockno, unsigned int *buf);

/* Raw drive/controller status (DIB), for a caller that wants to check
 * Ready/Busy/Reserved/fault bits before issuing a command -- see
 * zebra.c's ZEBRA_DIB_* masks. */
unsigned int zebra_status(unsigned int unit);

#endif
