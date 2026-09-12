#ifndef _DSKP_COMMON_H
#define _DSKP_COMMON_H

/* dskp_common.h / dskp_common.c -- shared register-level core for
 * Data General's "DG/Disc Storage Subsystem" family, mnemonic DSKP,
 * device select code 027 octal (alternate 067 octal), as documented
 * independently across three real DG manuals covering three different
 * hardware generations the user owns:
 *
 *   - Zebra   (DG 6060/6061/6067, 1976-1980) -- ZEBRA_NOTES.md
 *   - Vulcan  (DG 6122, 1979-1980)           -- VULCAN_NOTES.md
 *   - Kismet  (DG 6160/6161/6214, 1981-1982) -- KISMET_NOTES.md
 *
 * Three independent scoping passes on this codebase each transcribed
 * their own generation's manual from page images and each separately
 * discovered the identical device select code and DSKP register
 * convention -- Data General reused one standing programming model
 * across a decade of hardware generations rather than inventing a new
 * one each time. See DSKP_FAMILY_NOTES.md at the repo root for the
 * full unification writeup (what's genuinely identical across all
 * three vs. what's real, load-bearing, generation-specific
 * difference, cross-checked bit-by-bit against the three NOTES files
 * above while this file was built) -- this header only states the
 * conclusions; that file has the citation trail.
 *
 * *** UNVERIFIED ON A SIMULATOR OR REAL HARDWARE, ALL THREE GENERATIONS ***
 * This project's SIMH build does not model any DSKP-family controller.
 * Nothing in this file or any of the three drivers built on it has ever
 * executed -- compile/assemble-clean through the real eclipse-cc/dgasm
 * pipeline is the verification ceiling for all three, exactly as it was
 * before this unification. See each generation's own NOTES.md for the
 * per-generation "what was and wasn't checked" detail; unifying the
 * three drivers onto this shared core does not change that ceiling.
 *
 * --- Variant selection -----------------------------------------------
 *
 * Exactly one DSKP-family generation may be compiled into a given
 * program (this mirrors the real hardware: a system has one DSKP-class
 * controller installed, not several at once). The including .c file
 * (zebra.c, vulcan.c, or kismet.c) selects its generation by defining
 * exactly one of the following BEFORE #including this header:
 *
 *   #define DSKP_VARIANT_ZEBRA
 *   #define DSKP_VARIANT_VULCAN
 *   #define DSKP_VARIANT_KISMET
 *
 * Defining none, or more than one, in the same translation unit is a
 * compile-time #error below (a real, if narrow, guard: it catches, in
 * one translation unit, e.g. accidentally #including both zebra.h and
 * vulcan.h in the same .c file). It does NOT, and per this toolchain's
 * own real limits cannot, catch two *different* variant .c files (say
 * zebra.c and vulcan.c) both being passed to one `eclipse-cc` build --
 * eclipse-cc has no -D command-line passthrough and dgasm has no
 * separate-compilation/linking model of its own, so there is no
 * per-file compile-time signal one translation unit could check against
 * another's choice. That real cross-file case IS still caught, but at
 * the actual llvm-link whole-program merge step eclipse-cc's own
 * pipeline already runs before dgasm ever sees anything -- see
 * dskp_family_active_variant below, and DSKP_FAMILY_NOTES.md for the
 * empirical proof this actually fires.
 *
 * Exception: dskp_common.c itself (the shared core's own implementation
 * file) also #includes this header, to get prototypes matching its own
 * definitions -- but dskp_common.c is deliberately variant-AGNOSTIC
 * (every function it defines is identical regardless of which
 * generation is selected, and it never references DSKP_VARIANT_ID), so
 * it #defines DSKP_COMMON_CORE_TU before including this header, to skip
 * the "exactly one variant" requirement below -- it would otherwise
 * fail its OWN compile (dskp_common.c is compiled as its own separate
 * translation unit by eclipse-cc, so it has no visibility into which
 * variant .c file it'll eventually be linked alongside). */
#if defined(DSKP_COMMON_CORE_TU)
  /* dskp_common.c: no variant selection needed or expected. */
#elif defined(DSKP_VARIANT_ZEBRA)
#  if defined(DSKP_VARIANT_VULCAN) || defined(DSKP_VARIANT_KISMET)
#    error "dskp_common.h: more than one of DSKP_VARIANT_ZEBRA/_VULCAN/_KISMET is defined in this translation unit -- exactly one DSKP-family generation may be selected per build. See DSKP_FAMILY_NOTES.md."
#  endif
#  define DSKP_VARIANT_ID 1
#elif defined(DSKP_VARIANT_VULCAN)
#  if defined(DSKP_VARIANT_KISMET)
#    error "dskp_common.h: more than one of DSKP_VARIANT_ZEBRA/_VULCAN/_KISMET is defined in this translation unit -- exactly one DSKP-family generation may be selected per build. See DSKP_FAMILY_NOTES.md."
#  endif
#  define DSKP_VARIANT_ID 2
#elif defined(DSKP_VARIANT_KISMET)
#  define DSKP_VARIANT_ID 3
#else
#  error "dskp_common.h: exactly one of DSKP_VARIANT_ZEBRA / DSKP_VARIANT_VULCAN / DSKP_VARIANT_KISMET must be #defined by the including .c file before #including dskp_common.h, to select which DG DSKP-family generation this build targets. See DSKP_FAMILY_NOTES.md."
#endif

/* dskp_family_active_variant -- deliberately the SAME global symbol
 * name in all three variant .c files (each defines it once, set to its
 * own DSKP_VARIANT_ID). This is the real, working, whole-program guard
 * against two variants being linked into one program: eclipse-cc's own
 * pipeline runs `llvm-link` to merge every input .c file's IR into one
 * module before dgasm ever sees it (see eclipse-cc's own header comment
 * -- "dgasm has no separate-compilation/linking model" is exactly why
 * this whole-program merge happens first) -- and llvm-link rejects two
 * modules that each define the same *external* (non-static) global,
 * confirmed directly against the real toolchain while building this
 * file:
 *
 *   $ eclipse-cc -o x.simh zz_a.c zz_b.c main.c
 *   error: Linking globals named 'dskp_family_selected_variant':
 *          symbol multiply defined!
 *
 * (a throwaway pair of test files, `zz_a.c`/`zz_b.c`, each defining a
 * same-named external global with a different value, scratch-only, not
 * committed -- see DSKP_FAMILY_NOTES.md for the full transcript and the
 * real test against `dskp_family_active_variant` itself). So: linking,
 * say, zebra.c and vulcan.c into one program now fails
 * loudly at the llvm-link stage, before dgasm even runs -- not because
 * of the original DSKP symbol collision (that's fixed too, see
 * dskp_common.c) but because of this deliberate sentinel. */
extern int dskp_family_active_variant;

/* --- Device identity, DSKP_FAMILY_NOTES.md -- identical across all
 * three generations, confirmed in all three manuals independently. --- */
#define DSKP_DEVICE_CODE      027u /* primary select code, octal */
#define DSKP_DEVICE_ALT_CODE  067u /* alternate/jumper code -- documented
                                     * by all three manuals, not used
                                     * (hard-coded to the primary code)
                                     * by any of the three drivers, same
                                     * as before unification. */
#define DSKP_PRIORITY_MASK_BIT 7

/* --- Shared 16-entry command table: DOA bits 5-8. Byte-for-byte
 * identical bit position and command assignment in all three manuals
 * (ZEBRA_NOTES.md, VULCAN_NOTES.md, KISMET_NOTES.md's own DOA command
 * tables) -- Kismet's own manual marks 0011-1000 "Reserved" rather than
 * naming STOP/OFFSET FWD/OFFSET REV/WRITE DISABLE/RELEASE/TRESPASS
 * individually the way Zebra/Vulcan's do, but the values that ARE named
 * (READ/RECAL/SEEK, ALT MODE 1/2, NOP, VERIFY, READ BUFFERS, WRITE,
 * FORMAT) match exactly -- so the full 16-entry table below is shared,
 * with a note that Kismet's own manual doesn't individually confirm
 * every one of the STOP/OFFSET/WRITE DISABLE/RELEASE/TRESPASS codes
 * (see DSKP_FAMILY_NOTES.md). --- */
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

/* DOA word helper for the "select drive + command" pattern common to
 * every phase (Phase I select+seek, Phase III select+read/write) in
 * all three generations: bit0 = Clear R/W Done (set unconditionally,
 * same choice all three original drivers already made -- Vulcan/
 * Kismet's own manuals confirm this bit's meaning directly; Zebra's
 * manual leaves the bit unlabeled beyond "R/W", borrowed by analogy,
 * see ZEBRA_NOTES.md), bits5-8 = command. Drive-select field WIDTH and
 * POSITION is deliberately NOT included here -- it's real,
 * generation-specific difference (Zebra/Vulcan: 2-bit drive 0-3 at
 * bits9-10; Kismet: 1-bit drive 0-1 at bit10 only, bit9 must be 0) --
 * see DSKP_FAMILY_NOTES.md. Each variant ORs its own drive-select bits
 * into the result. */
#define DSKP_DOA_CLR_RW_DONE 0x8000u /* bit 0 */
#define DSKP_DOA_CMD(cmd) (DSKP_DOA_CLR_RW_DONE | (((unsigned int)(cmd) & 0xFu) << 7))

/* --- DIA (Read Data Transfer Status) fault-bit field, bits 6-15.
 * Confirmed IDENTICAL bit position and meaning in all three manuals,
 * transcribed independently by all three original scoping efforts and
 * cross-checked line-by-line against each other while building this
 * file -- this is the single strongest piece of evidence that all
 * three generations share one real register convention, not just a
 * device-code coincidence. Bit 11's own name is the one place the three
 * manuals use different English words for the identical bit ("SEC/HD
 * ERR" Zebra, "Surf/sect error" Vulcan, "Head/sect error" Kismet) --
 * same bit, same fault condition, different generation's own
 * terminology for the surface-or-head addressing axis; DSKP_FAMILY_NOTES.md
 * has the three quotes side by side. Bits 0-5 are NOT shared here (see
 * below -- real, generation-specific layout differences). --- */
#define DSKP_DIA_PARITY_ERROR   0x0200u /* bit 6 */
#define DSKP_DIA_INVALID_SECTOR 0x0100u /* bit 7 */
#define DSKP_DIA_ECC_ERROR      0x0080u /* bit 8 */
#define DSKP_DIA_BAD_SECTOR     0x0040u /* bit 9 */
#define DSKP_DIA_CYLINDER_ERROR 0x0020u /* bit 10 */
#define DSKP_DIA_HDSECT_ERROR   0x0010u /* bit 11 */
#define DSKP_DIA_VERIFY_ERROR   0x0008u /* bit 12 */
#define DSKP_DIA_RW_TIMEOUT     0x0004u /* bit 13 */
#define DSKP_DIA_DATA_LATE      0x0002u /* bit 14 */
#define DSKP_DIA_RW_FAULT       0x0001u /* bit 15: OR of all of the above,
                                          * or a drive fault on the
                                          * currently selected drive */
#define DSKP_DIA_ALLERR         0x03FFu /* bits 6-15 */
#define DSKP_DIA_HAS_FAULT(dia) ((unsigned int)(dia) & DSKP_DIA_ALLERR)

/* --- DIB (Read Drive Status) bits -- only the FIVE bit positions
 * confirmed identical, by bit position and meaning, in all three
 * manuals are shared here: Ready, Busy, Write Disabled, Positioner
 * Fault, Drive Fault. Zebra's and Vulcan's own manuals additionally
 * document Invalid Status(0)/Reserved(1)/Trespassed(2)/Offset(5)/
 * Invalid Address(8)/Illegal Command(9)/Power Fault(10)/Pack Unsafe(11)/
 * Clock Fault(13)/Write Fault(14) at the SAME bit positions as each
 * other (a real two-generation cross-check on its own), and Vulcan
 * alone additionally documents a "Drive ID" bit at bit 7 (identifies
 * the drive as a model 6122) that neither Zebra's nor Kismet's manual
 * mentions. Kismet's own manual, by contrast, documents only these
 * five bits for DIB's default context at all -- not fewer bits on the
 * hardware necessarily, just a sparser table in that specific manual
 * (KISMET_NOTES.md's own DIB table has real gaps at bits 0-2,5,7-11,
 * 13-14, simply undocumented rather than stated reserved). Each
 * variant's own header carries its own generation-specific extra bits
 * on top of these five -- see zebra.h/vulcan.h. --- */
#define DSKP_DIB_READY             0x1000u /* bit 3 */
#define DSKP_DIB_BUSY              0x0800u /* bit 4 */
#define DSKP_DIB_WRITE_DISABLED    0x0200u /* bit 6 */
#define DSKP_DIB_POSITIONER_FAULT  0x0008u /* bit 12 */
#define DSKP_DIB_DRIVE_FAULT       0x0001u /* bit 15 */

/* --- S, C, P flag-command semantics -- word-for-word the same
 * substance in all three manuals (ZEBRA_NOTES.md/VULCAN_NOTES.md/
 * KISMET_NOTES.md each quote their own manual's version verbatim; the
 * three quotes differ only in which exact command list f=S/f=P name,
 * itself just the same READ/WRITE/FORMAT/READ BUFFERS/VERIFY vs.
 * SEEK/RECAL/OFFSET/STOP/WRITE DISABLE/RELEASE/TRESPASS split every
 * generation shares):
 *
 *   f=S  Sets Busy=1, Done=0. Starts: READ, WRITE, FORMAT, READ
 *        BUFFERS, VERIFY.
 *   f=C  Sets Busy=0, Done=0. Stops all data-transfer operations.
 *   f=P  Starts: SEEK, RECALIBRATE, OFFSET, STOP, WRITE DISABLE,
 *        RELEASE, TRESPASS. Does NOT affect the Busy flag or Done flag.
 *
 * The genuine, real family-wide consequence: a P-pulsed SEEK is never
 * visible to a SKPDN/SKPBN poll -- only the DIA/DIB mechanisms each
 * generation itself documents (Zebra: per-drive DIA attention bits,
 * explicitly polled; Vulcan/Kismet: no software poll at all, the
 * *controller* is documented to defer the next stored read/write
 * command internally until the seek it already knows about finishes).
 * This is real command-SEQUENCING difference, not just register-layout
 * difference -- kept entirely in each variant's own file, not
 * abstracted here. See DSKP_FAMILY_NOTES.md. --- */

/* --- The shared 4-phase-ish protocol shape, and the DOC context-
 * sensitivity, are ALSO family-wide, not Kismet-specific: all three
 * manuals independently document that DOC's meaning depends on whether
 * the immediately preceding DOA specified SEEK ("Specify Cylinder", 10
 * bits, bits 6-15 -- see dskp_docp_seek() below, identical field
 * position in all three) or something else ("Specify Surface/Head,
 * Sector and Count" -- Zebra fits this in ONE DOC since its narrower
 * 5-bit surface/sector/count fields don't need an MSB split; Vulcan and
 * Kismet both need a first, extra DOC carrying MSBs before it, a real,
 * shared VULCAN/KISMET-specific structural difference from Zebra --
 * see DSKP_FAMILY_NOTES.md and each variant's own file). This header
 * deliberately does NOT try to force a single generic "the DOC(s)"
 * abstraction across all three, because the actual field layouts differ
 * enough (1-DOC vs 2-DOC, which fields get an MSB split, surface vs.
 * head terminology, field widths) that forcing one shape here would
 * either lose a real difference or produce something no single manual
 * actually describes -- each variant's own file builds its own
 * DOC word(s) directly from its own geometry, using the shared DIA/DIB/
 * command constants above. --- */

/* --- Shared low-level register primitives. Byte-for-byte the same
 * instruction sequences all three original drivers already had
 * (dskp_doa/dskp_doc/dskp_dia/dskp_dib were each independently written,
 * nearly identically, three times -- see DSKP_FAMILY_NOTES.md for the
 * side-by-side) -- now defined exactly once, in dskp_common.c, against
 * the symbolic `DSKP` device name (confirmed empirically to assemble
 * correctly combined with a dynamic "r"-constrained %-operand, a
 * combination none of the three original files had tried -- see
 * DSKP_FAMILY_NOTES.md) rather than each file pasting the literal text
 * "027" independently.
 *
 * Bare (unpulsed) DOA/DOC/DOB loads and bare DIA/DIB reads: pulse
 * discipline identical to all three original drivers' own stated
 * reasoning (S/C/P flag commands act on the *device*, not on whichever
 * specific register-load instruction happens to carry the pulse, so a
 * multi-register command sequence must keep the pulse off every step
 * but the last) -- confirmed for DSK's simulated hardware by reading
 * nova_dsk.c directly (STORAGE_NOTES.md), assumed by analogy (not
 * independently confirmed against source, since no simulator models
 * DSKP) for all three DSKP generations, exactly as before. */
void dskp_doa(unsigned int word);
void dskp_doc(unsigned int word);
void dskp_dob(unsigned int word);
unsigned int dskp_dia(void); /* does not clear status */
unsigned int dskp_dib(void); /* does not clear status */

/* Specify Cylinder (bits 6-15, 10 bits -- identical field position in
 * all three manuals) + P pulse, combined: starts a SEEK. Per the S/C/P
 * semantics above, nothing about this pulse can be polled via
 * SKPDN/SKPBN -- each variant's own file decides how it detects seek
 * completion (or, for Vulcan/Kismet, deliberately doesn't wait at all,
 * relying on the controller's own documented internal deferral). */
void dskp_docp_seek(unsigned int cylinder);

/* Specify Memory Address (bit0 = EMA LSB, left 0 by every variant --
 * no extended/BMC-mapped addressing is used by any of the three
 * drivers, same restriction as before unification; bits1-15 = address)
 * + S pulse, combined, then poll SKPDN to completion. Starts READ,
 * WRITE, FORMAT, READ BUFFERS, or VERIFY depending on which command the
 * most recent select+command DOA loaded. Structurally identical to
 * eclipse_io.h's outb() macro (DOBS + SKPDN wait loop) -- written out
 * directly here, as all three original drivers already did, rather than
 * routed through that macro, so this file doesn't depend on
 * IO_CHECK_DEVICE_'s stringize-of-a-literal-device-code working for a
 * symbolic `DSKP` name (it wouldn't -- IO_CHECK_DEVICE_ range-checks an
 * octal literal via `_Static_assert((device) >= 0 ...)`, which requires
 * an integer constant expression; a symbolic assembler-level device name
 * like `DSKP` is not one). */
void dskp_dobs_start(unsigned int mem_addr_word);

#endif
