#ifndef _DSK_H
#define _DSK_H

#include "blockdev.h"

/* dsk.h -- C-callable driver for the DSK device (device select code 020
 * octal, the DG 4019 fixed-head disk simulator -- nova_dsk.c's own
 * header comment, see STORAGE_NOTES.md for the full identity/capacity
 * citation trail). This is the one real, hardware/simulator-verified
 * backend behind blockdev.h (examples/blockdev.h) -- built directly on
 * the exact DOA/DOB/NIOS/NIOP/SKPDN register sequence
 * examples/disk_probe.s already proved correct by hand-assembly, now as
 * reusable C-callable functions instead of a one-off probe.
 *
 * DSK is a single-unit device (unlike DKP/Vulcan/Kismet/Zebra, each of
 * which is a real multi-drive controller) -- no `unit` argument, unlike
 * those other drivers' own foo_read_block(unit, blockno, buf) shape.
 *
 * Scope, deliberately matching disk_probe.s's own already-verified
 * scope, not expanded here: one 256-word sector per call, polled
 * completion (SKPDN), coarse status (raw DSKS_ALLERR bits, 0 = no
 * error -- not decoded further; a caller wanting the individual
 * write-lock/data-late/nonexistent-disk/CRC bits can mask the returned
 * value itself against nova_dsk.c's DSKS_* constants), `buf` must be a
 * plain, non-extended pointer (0-32767 range) -- same restriction
 * STORAGE_NOTES.md already documented for this device.
 */

#define DSK_WORDS_PER_SECTOR 256u
/* Default 1-platter (262KW) size: 262144 / 256 = 1024 blocks, 0-1023.
 * See STORAGE_NOTES.md -- DSK is expandable to 8 platters on the same
 * unit ("1P".."8P"), which would raise this; not assumed here since
 * disk_probe.s's own verification (and this file's) was against the
 * default 1-platter size only. */
#define DSK_MAX_BLOCK 1023u

/* dsk_read_block / dsk_write_block: transfer exactly one 256-word
 * sector between the DSK device and `buf`. `blockno` is DSK's own flat
 * block address (0..DSK_MAX_BLOCK) -- no cylinder/head/sector
 * translation needed; DSK already presents a flat linear block space in
 * hardware (STORAGE_NOTES.md).
 *
 * Return value: 0 = success (DIA read back 0, no error bits set after
 * the transfer completed). -1 = blockno out of range (checked in
 * software before touching the device at all -- disk_probe.s's own
 * probe never needed this since it always used a fixed, known-good
 * blockno). Any other nonzero value is the raw DSKS_ALLERR-masked
 * status word DIA returned -- see nova_dsk.c for what each bit means;
 * not decoded further here, matching STORAGE_NOTES.md's own stated
 * scope for what a first driver still needs ("error handling is
 * unexercised").
 */
int dsk_read_block(unsigned int blockno, unsigned int *buf);
int dsk_write_block(unsigned int blockno, unsigned int *buf);

/* dsk_blockdev_read / dsk_blockdev_write: adapters conforming
 * dsk_read_block/dsk_write_block's own (blockno, buf) shape to
 * blockdev.h's generic (ctx, blockno, buf) dispatch shape. `ctx` is
 * ignored -- DSK is a single-unit device, so there is nothing for a
 * context value to select between. See blockdev.h's own "Plugging in a
 * future backend" note for why a multi-unit backend's adapters would
 * use `ctx` instead of ignoring it.
 */
int dsk_blockdev_read(void *ctx, unsigned int blockno, void *buf);
int dsk_blockdev_write(void *ctx, unsigned int blockno, void *buf);

/* dsk_ops: this backend's blockdev_ops_t, wired to the two adapters
 * above. Declared here (not just built inline where it's used) so any
 * caller can build a `blockdev_t` for DSK with `blockdev_t dsk_dev =
 * { &dsk_ops, 0 };` -- see blockdev_test.c. */
extern const blockdev_ops_t dsk_ops;

#endif
