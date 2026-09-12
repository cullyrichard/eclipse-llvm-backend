#include "blockdev.h"

/* blockdev_read/blockdev_write: dispatch through dev's function-pointer
 * fields. See blockdev.h's header comment for why this indirection is
 * proven safe on this backend (DEBUGGING_NOTES.md entry #17), and
 * examples/blockdev_test.c for this project's own direct re-verification
 * of it against the real DSK backend.
 */
int blockdev_read(blockdev_t *dev, unsigned int blockno, void *buf) {
    return dev->ops->read_block(dev->ctx, blockno, buf);
}

int blockdev_write(blockdev_t *dev, unsigned int blockno, void *buf) {
    return dev->ops->write_block(dev->ctx, blockno, buf);
}
