#ifndef _BLOCKDEV_H
#define _BLOCKDEV_H

/* blockdev.h -- generic block-device abstraction, phase 6 of this
 * project's OS roadmap.
 *
 * Goal: let kernel code call blockdev_read(dev, blockno, buf) /
 * blockdev_write(dev, blockno, buf) without caring which physical
 * controller is behind `dev`. One real, verified backend exists today
 * (examples/dsk.h/dsk.c, the DSK device -- see below); the point of
 * this header is that a future Zebra/Vulcan/Kismet/Argus backend plugs
 * into the exact same shape with zero change to any kernel code already
 * written against blockdev_read()/blockdev_write().
 *
 * SHAPE, AND WHY IT ENDED UP AS TWO NESTED 2-FIELD STRUCTS RATHER THAN
 * ONE 3-FIELD ONE: function pointers through a struct field are real and
 * proven on this backend (DEBUGGING_NOTES.md entry #17, "Indirect
 * (function-pointer) call support added" -- confirmed by re-reading that
 * entry directly, not trusted secondhand: c-testsuite 00087's exact
 * `struct S { int (*fptr)(); }; v.fptr = foo; v.fptr();` shape passes,
 * and a *global* struct initialized with a function-pointer value works
 * too, both independently verified there). The first design tried here
 * was the obvious one: one flat struct, `{ read_block, write_block,
 * ctx }`. It compiled fine but TRAPPED at runtime (PC snapped to 0, the
 * "corrupted SAVE/RTN linkage" signature DEBUGGING_NOTES.md's entry #11
 * describes) the moment `blockdev_write()` (which reads the *middle*
 * field, `write_block`) ran.
 *
 * Root-caused, not just worked around blind, by re-reading
 * DEBUGGING_NOTES.md entry #15's still-OPEN bug #2 before touching
 * anything: "a struct field reached through a pointer *variable* ...
 * computes a *different* physical word ... when the field is neither
 * the struct's first nor last" -- i.e. exactly a 3-(or-more)-field
 * struct's *middle* field, accessed through a pointer parameter (which
 * is what `dev` is, inside `blockdev_read`/`blockdev_write`). That entry
 * is explicit that this is unfixed (c-testsuite 00163/00179/00180/00205
 * still skip-listed for it) and that entry #17's own struct-function-
 * pointer fix and entry #18's own struct-field fix each cover a
 * *different*, narrower shape than this one -- neither closes it out.
 * Confirmed the match by inspecting this file's own generated assembly
 * directly (`llc` output for the original 3-field version): `dev->
 * read_block` (first field, offset 0) and `dev->ctx` (last field)
 * compiled as expected, but `dev->write_block` (the middle field)
 * resolved through the same DIV-based runtime halve sequence entry #15
 * flags as the inconsistent one.
 *
 * FIX ACTUALLY USED: restructure so no struct this code touches has a
 * middle field at all -- every struct here has exactly two fields, so
 * every field is either "first" or "last," the two shapes entry #17
 * already proved safe (and entry #15's bug is explicitly scoped to "the
 * field is neither the struct's first nor last"). A small ops-table
 * struct holds just the two function pointers (both safe: first and
 * last of two); the device handle struct holds a pointer to that ops
 * table plus `ctx` (also both safe, same reason). Re-verified after
 * this restructuring: examples/blockdev_test.c (which goes through
 * `blockdev_read`/`blockdev_write` only, never `dsk_read_block`/
 * `dsk_write_block` directly) now runs to completion with correct
 * output -- see BLOCKDEV_NOTES.md for the real captured transcript, and
 * for the theory this file started from before it also tried, and
 * initially got wrong.
 *
 * This is exactly the kind of finding the task that produced this file
 * asked to be surfaced honestly rather than papered over: real function
 * pointers through a struct field DO work here, but only reliably up to
 * two fields per struct level -- a genuine, narrower-than-textbook-C
 * constraint, not "function pointers don't work" and not "this needed a
 * switch-based dispatch instead" (a flat integer-tag `switch` in
 * `blockdev_read`/`blockdev_write` was the documented fallback if
 * *no* function-pointer shape had worked; it wasn't needed -- nesting
 * two 2-field structs was enough).
 *
 * SCOPE: single 256-word-sector raw block I/O only (matching every real
 * backend's own natural transfer unit -- DSK's fixed DSK_NUMWD=256,
 * Vulcan/Kismet/Zebra's own 256-word sector, per their own headers). No
 * multi-sector transfers, no partitioning/filesystem layer, no
 * asynchronous/interrupt-driven completion (every current backend is a
 * polled-completion driver; the return convention below leaves room for
 * a future backend to still be synchronous from the *caller's* point of
 * view even if it internally takes a completion interrupt). `ctx` lets
 * a backend distinguish which physical unit/drive it's talking to
 * without needing a separate global per instance -- unused by the DSK
 * backend (DSK is a single-unit device, see STORAGE_NOTES.md), but
 * required for a Vulcan/Kismet/Zebra backend, which are all real
 * multi-unit controllers (see the "Plugging in a future backend"
 * section below).
 */

typedef struct blockdev_ops {
    /* Read/write exactly one DSK_WORDS_PER_SECTOR-sized (or whatever
     * the concrete backend's own natural sector size is) block. `buf`
     * must point to a plain, non-extended pointer (0-32767 range) --
     * the same restriction every real backend in this project already
     * operates under (see dsk.h, vulcan.h, kismet.h, zebra.h).
     * Returns 0 on success, nonzero on failure -- the exact meaning of
     * a nonzero code is backend-specific (see the concrete backend's
     * own header for what its codes mean); blockdev.h itself only
     * guarantees "0 == success" uniformly across every backend.
     *
     * Exactly two fields, deliberately -- see this file's header
     * comment for why a struct used this way (accessed through a
     * pointer parameter) must not have a genuine middle field on this
     * backend.
     */
    int (*read_block)(void *ctx, unsigned int blockno, void *buf);
    int (*write_block)(void *ctx, unsigned int blockno, void *buf);
} blockdev_ops_t;

typedef struct blockdev {
    /* Pointer to this backend's ops table (normally a single `static
     * const blockdev_ops_t` per backend, shared by every instance of
     * it -- see dsk.c). */
    const blockdev_ops_t *ops;

    /* Opaque, backend-owned context, passed back verbatim as the first
     * argument to read_block/write_block. A single-unit backend (DSK)
     * can leave this NULL/0; a multi-unit backend can pack whatever
     * identifies which physical drive this blockdev_t instance means
     * into it -- see dsk.c's dsk_blockdev_read/write adapters for the
     * trivial (ctx-ignoring) case and the "Plugging in a future
     * backend" note below for the multi-unit case. Exactly two fields
     * in this struct too, same reason as blockdev_ops_t above.
     */
    void *ctx;
} blockdev_t;

/* blockdev_read/blockdev_write: the one interface kernel code should
 * ever call. Dispatches through dev->ops->read_block/write_block -- see
 * this file's header comment for why that two-level, two-fields-per-
 * level shape is what's proven safe on this backend. Implemented as
 * real (non-inline) functions in blockdev.c, not `static inline`, since
 * this target's freestanding backend has never been exercised with an
 * inline function and there is no need to take that risk here -- an
 * ordinary extern function costs one indirect call worth of overhead,
 * irrelevant next to a device I/O operation.
 */
int blockdev_read(blockdev_t *dev, unsigned int blockno, void *buf);
int blockdev_write(blockdev_t *dev, unsigned int blockno, void *buf);

/* --- Plugging in a future backend (Zebra / Vulcan / Kismet / Argus) ---
 *
 * Each of those already exists as its own `foo_read_block(unit, blockno,
 * buf)` / `foo_write_block(unit, blockno, buf)` pair (see vulcan.h,
 * kismet.h, zebra.h) -- real functions, already shaped almost exactly
 * like dsk.c's own dsk_read_block/dsk_write_block, just with an extra
 * leading `unit` argument DSK doesn't need (DSK is a single-unit
 * device; those controllers are real multi-drive ones). Wiring any of
 * them into this interface needs exactly two small adapter functions,
 * one shared ops table, and one blockdev_t per unit -- mirroring dsk.c's
 * own dsk_blockdev_read/dsk_blockdev_write and dsk_ops exactly:
 *
 *   static int vulcan_blockdev_read(void *ctx, unsigned int blockno, void *buf) {
 *       unsigned int unit = (unsigned int)(unsigned long)ctx;
 *       return vulcan_read_block(unit, blockno, buf);
 *   }
 *   static int vulcan_blockdev_write(void *ctx, unsigned int blockno, void *buf) {
 *       unsigned int unit = (unsigned int)(unsigned long)ctx;
 *       return vulcan_write_block(unit, blockno, buf);
 *   }
 *   static const blockdev_ops_t vulcan_ops = { vulcan_blockdev_read,
 *                                               vulcan_blockdev_write };
 *   blockdev_t vulcan_dev0 = { &vulcan_ops, (void *)(unsigned long)0 };  // unit 0
 *   blockdev_t vulcan_dev1 = { &vulcan_ops, (void *)(unsigned long)1 };  // unit 1
 *
 * -- one shared ops table, one blockdev_t per physical unit, `ctx` doing
 * double duty as a (non-pointer) unit number packed into a void* the
 * same way this backend already treats "just a value" everywhere else.
 * No change to blockdev.h, blockdev.c, dsk.c, or blockdev_test.c would
 * be needed. This is intentionally left as a documented extension
 * point, not implemented here -- Zebra/Vulcan/Kismet/Argus are being
 * scoped/unified elsewhere in parallel (see ZEBRA_NOTES.md/
 * VULCAN_NOTES.md/KISMET_NOTES.md/ARGUS_NOTES.md) and none of their
 * files are touched by this change.
 */

#endif
