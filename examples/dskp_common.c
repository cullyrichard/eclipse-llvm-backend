#define DSKP_COMMON_CORE_TU
#include "dskp_common.h"

/* See dskp_common.h's header comment before trusting any of this
 * against real Zebra/Vulcan/Kismet hardware -- this has never been
 * executed, only compiled/assembled (see DSKP_FAMILY_NOTES.md).
 *
 * The ONE canonical "dev DSKP = <code>" declaration for the whole DSKP
 * family core. This is the actual fix for the real, empirically-
 * reproduced "Multiple definitions for symbol DSKP" conflict
 * KISMET_NOTES.md documented (see DSKP_FAMILY_NOTES.md for the
 * before/after transcript: two translation units each independently
 * declaring `dev DSKP = 027` -- the shape kismet.c's own file-scope
 * `asm("dev DSKP = 027")` used before this refactor -- collide at the
 * dgasm step; exactly one declaration, anywhere in the whole linked
 * program, does not). dskp_common.c is meant to be compiled into every
 * DSKP-family build, paired with exactly one of zebra.c/vulcan.c/
 * kismet.c (never more than one -- see dskp_common.h's
 * dskp_family_active_variant sentinel for the real, verified guard
 * against that), so this line is emitted exactly once per program by
 * construction.
 *
 * Register-load instructions (DOA/DOC/DOB, bare, no pulse) all have a
 * genuine per-instruction accumulator operand, so this file uses
 * ordinary "r"-constrained inline-asm operands throughout -- the same
 * pattern examples/zebra.c and examples/vulcan.c already used for their
 * own (now-consolidated) copies of these exact functions. Unlike those
 * two files, this one references the device by its symbolic `DSKP` name
 * rather than pasting the literal text "027" into every instruction --
 * confirmed empirically, while building this unification, that dgasm
 * accepts a symbolic device name combined with a dynamic "r"-constrained
 * %-operand in the same instruction (a combination none of the three
 * original files had actually tried: zebra.c/vulcan.c used "r"-operands
 * but pasted a literal device code specifically to avoid this question;
 * kismet.c used the symbolic name but only ever with a fixed, literal
 * AC operand via ELDA/ESTA-loaded globals, never together with a
 * dynamic register operand) -- see DSKP_FAMILY_NOTES.md for the test
 * transcript. This lets the device code live in exactly one place
 * (this line) instead of being pasted as a magic literal in over a
 * dozen places across three files, as it was before this refactor.
 */
asm("dev DSKP = 027");

/* dskp_family_active_variant -- see dskp_common.h's own comment on this
 * symbol. Each variant .c file defines it once, to its own
 * DSKP_VARIANT_ID; if two variant .c files are ever linked into one
 * program by mistake, llvm-link itself rejects the build (confirmed
 * empirically -- DSKP_FAMILY_NOTES.md). dskp_common.c does NOT define
 * it -- it is genuinely variant-specific, and dskp_common.c itself is
 * variant-agnostic (every function below is identical regardless of
 * which generation is selected). */

/* --- Bare (unpulsed) register loads and readbacks -- see
 * dskp_common.h's own comment on pulse discipline. --- */
void dskp_doa(unsigned int word) {
    asm volatile("DOA %0,DSKP" :: "r"(word));
}
void dskp_doc(unsigned int word) {
    asm volatile("DOC %0,DSKP" :: "r"(word));
}
void dskp_dob(unsigned int word) {
    asm volatile("DOB %0,DSKP" :: "r"(word));
}
/* Bare DIA/DIB -- does not clear status. Several callers (all three
 * variants' own Phase I/III control-full or ready checks) re-read the
 * same not-yet-cleared status more than once; a clearing read would be
 * wrong here, same reasoning disk_probe.s already established for DSK's
 * DIA (STORAGE_NOTES.md). */
unsigned int dskp_dia(void) {
    unsigned int r;
    asm volatile("DIA %0,DSKP" : "=r"(r));
    return r;
}
unsigned int dskp_dib(void) {
    unsigned int r;
    asm volatile("DIB %0,DSKP" : "=r"(r));
    return r;
}

/* Specify Cylinder + P pulse, combined -- starts a SEEK. Per the S/C/P
 * semantics dskp_common.h documents, f=P "does not affect the Busy flag
 * or Done flag" in all three manuals, so nothing here can be polled via
 * SKPDN/SKPBN; each variant's own file decides how (or whether) it
 * waits for seek completion. */
void dskp_docp_seek(unsigned int cylinder) {
    asm volatile("DOCP %0,DSKP" :: "r"(cylinder & 0x03FFu));
}

/* Specify Memory Address + S pulse, combined, then poll for Done.
 * Structurally identical to eclipse_io.h's outb() macro (DOBS + SKPDN
 * wait loop) and to all three original drivers' own
 * dskp_dobs_start()/outb-equivalent -- consolidated here verbatim. */
void dskp_dobs_start(unsigned int mem_addr_word) {
    asm volatile(
        "DOBS %0,DSKP\n\t"
        "dskp_rw_wait%=:\n\t"
        "SKPDN DSKP\n\t"
        "JMP dskp_rw_wait%=\n\t"
        :: "r"(mem_addr_word));
}
