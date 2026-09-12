#include "kismet.h"

/* kismet.c -- see kismet.h and KISMET_NOTES.md for the full picture.
 * This is UNVERIFIED against any running DSKP device: this project's
 * SIMH build does not model Kismet, so unlike examples/mmpu.c and
 * examples/disk_probe.s, nothing here has been single-stepped against
 * real or simulated hardware. What *has* been checked (see
 * KISMET_NOTES.md's verification section): this file compiles through
 * `eclipse-cc`'s real pipeline (clang -cc1 -> llvm-link -> opt -> llc
 * -> reorder_asm.py) and the resulting assembly assembles cleanly
 * with the real `dgasm -t eclipse_s140`, including every DOA/DOB/DOC/
 * DIA/DIB/DIC/SKPDN/SKPBN mnemonic and channel+pulse combination
 * (DOAS/DOAC/DOAP/DOBS/.../DIAC/DIBC/DICC) this file uses -- confirmed
 * directly against the real dgasm binary (~/dev/dgasm-src/dgasm),
 * independent of any specific device model, since dgasm's mnemonic
 * grammar doesn't know or care what device code a `dev` line names.
 *
 * Follows the same inline-asm discipline examples/mmpu.c established:
 * every value crosses the C/asm boundary through a named global (never
 * through "r"-constrained asm operands), because dgasm's `DOA ac,dev`-
 * style instructions take a fixed AC operand slot the way LMP does not
 * (LMP hardcodes AC0-2, DOA/DOB/DOC/DIA/DIB/DIC accept whichever AC you
 * name). Unlike mmpu.c, this file never touches AC2 or AC3 at all --
 * every DOx/DIx instruction below uses AC0 exclusively, reloaded fresh
 * from memory before each one -- so, unlike mmpu.c's mmpu_read_far/
 * mmpu_write_far, there is no need to save/restore the compiler's live
 * frame pointer (AC2) around these asm blocks. (This is a narrower
 * claim than mmpu.c's: it says AC0 is safe to clobber for the duration
 * of one of *these* asm blocks, which matches every existing example
 * in this codebase that uses AC0 as scratch (mmpu.c's own physpage/
 * offset handling, disk_probe.s's DSK register loads) -- it does not
 * re-verify mmpu.c's own AC2 finding.)
 */

/* asm("dev DSKP = 027") -- see the file-scope-asm rationale in
 * mmpu.c's own header comment (emitted unconditionally regardless of
 * which functions in this file survive dead-code elimination; exactly
 * one declaration, not one per function, or dgasm errors with
 * "Multiple definitions for symbol DSKP"). Device code 027 octal is
 * the *primary* DSKP select code (Programmer's Reference rev 1 p.4,
 * "Programming Summary": "Device code 27(Alt. 67)") -- the manual
 * documents an alternate 067 octal, jumper-selectable on the
 * controller for systems where 027 collides with something else; not
 * used here. */
asm("dev DSKP = 027");

/* Command register values (Programmer's Reference rev 1 p.5, DOA bits
 * 5-8), pre-shifted into DOA's bit position (bits 5-8 of a 16-bit
 * word = shift left 7 from the raw 4-bit code). */
#define KISMET_CMD_READ  (0000 << 7)
#define KISMET_CMD_SEEK  (0002 << 7)
#define KISMET_CMD_WRITE (0016 << 7)

/* All intermediate register words and the final status readback cross
 * the C/asm boundary through these globals -- see the file header
 * comment on why (nothing here survives in a register across an asm
 * block). */
static unsigned int _k_doa_seek;  /* Phase I: DOA (select drive + SEEK cmd) */
static unsigned int _k_dib_status; /* Phase I: DIB readback (ready/fault check) */
static unsigned int _k_doc_cyl;   /* Phase II: DOC (Specify Cylinder) + P pulse */
static unsigned int _k_doa_rw;    /* Phase III: DOA (select drive + READ/WRITE cmd) */
static unsigned int _k_doc1;      /* Phase IV: DOC (Specify Extended, Sector and Count, 1st) */
static unsigned int _k_doc2;      /* Phase IV: DOC (Specify Head, Sector and Count, 2nd) */
static unsigned int _k_dob;       /* Phase IV: DOB (Specify Memory Address) + S pulse */
static unsigned int _k_status;    /* final DIA readback */

/* Phase I only: select the drive, load the SEEK command, and read back
 * drive status (Ready/Busy/Write-disable/fault flags) without pulsing
 * anything -- lets kismet_rw_op() (below) decide in plain C whether to
 * proceed, before any seek or transfer is actually started. Mirrors
 * Figure 2's own "Phase I: Select a Drive and Specify a Seek Command"
 * flowchart (Programmer's Reference rev 1 p.11-12): DOA (select
 * drive+store command) -> DIB (inspect drive status) -> branch on
 * Ready. This DIB read does NOT clear anything (no pulse) -- same
 * assumption disk_probe.s made for DSK's un-pulsed DIA, though for
 * DSKP that assumption isn't confirmed against source (there is no
 * simulator source for this device); see KISMET_NOTES.md. */
static void kismet_select_and_check(int unit) {
    _k_doa_seek = (unit & 1) << 5 | KISMET_CMD_SEEK;
    asm volatile(
        "ELDA 0,_k_doa_seek,0\n\t"
        "DOA 0,DSKP\n\t"           /* bare: no pulse yet */
        "DIB 0,DSKP\n\t"           /* bare: read status, don't clear */
        "ESTA 0,_k_dib_status,0\n\t"
    );
}

/* Phases II-IV: start the seek (P pulse, does NOT touch the
 * controller's Busy/Done flags per the manual's own f=P description --
 * "Does not affect the Busy flag or Done flag"), immediately proceed
 * to select the drive + READ/WRITE command (Phase III's own text:
 * "If a read/write operation is to follow, proceed immediately to
 * Phase III without waiting for a drive attention interrupt request"),
 * load the extended sector/count + head/sector/count + memory address
 * registers, and pulse S to start the transfer. The controller itself
 * is documented to wait for the seek's Seek Busy flag to clear before
 * actually executing the stored read/write command (p.12's Phase IV
 * text) -- so this driver never polls DIB's per-drive Busy bit at all,
 * only the standard controller Busy/Done flag (SKPDN DSKP) for the
 * read/write's own completion, same generic Nova/Eclipse polled-
 * completion idiom STORAGE_NOTES.md's disk_probe.s already used for
 * DSK. */
static void kismet_seek_and_xfer(int unit, int cmd, int cyl, int head,
                                  int sector, void *buf) {
    unsigned int head_msb = (head >> 5) & 1;
    unsigned int sector_msb = (sector >> 5) & 1;
    /* Two's complement of a 1-sector transfer in the 6-bit count
     * field (bit 10 of the 1st DOC = MSB, bits 11-15 of the 2nd DOC =
     * low 5 bits): 64 - 1 = 63 decimal = 077 octal = all six bits set.
     * count_msb=1, count_low5=037 (all five low bits set). */
    unsigned int count_msb = 1;
    unsigned int count_low5 = 037;

    _k_doc_cyl = (unsigned int)cyl & 01777;
    _k_doa_rw = ((unsigned int)(unit & 1) << 5) | (unsigned int)cmd;
    _k_doc1 = (head_msb << 11) | (sector_msb << 10) | (count_msb << 5);
    _k_doc2 = (((unsigned int)head & 037) << 10) |
              (((unsigned int)sector & 037) << 5) |
              (count_low5 & 037);
    _k_dob = (unsigned int)buf & 077777;

    asm volatile(
        "ELDA 0,_k_doc_cyl,0\n\t"
        "DOCP 0,DSKP\n\t"          /* P pulse: starts the SEEK */
        "ELDA 0,_k_doa_rw,0\n\t"
        "DOA 0,DSKP\n\t"           /* bare: select drive + READ/WRITE cmd,
                                    *   per the manual proceed immediately,
                                    *   no wait for seek completion here */
        "ELDA 0,_k_doc1,0\n\t"
        "DOC 0,DSKP\n\t"           /* bare: Specify Extended, Sector and Count (1st) */
        "ELDA 0,_k_doc2,0\n\t"
        "DOC 0,DSKP\n\t"           /* bare: Specify Head, Sector and Count (2nd) */
        "ELDA 0,_k_dob,0\n\t"
        "DOBS 0,DSKP\n\t"          /* S pulse: sets mem addr AND starts the
                                    *   read/write (f=S per the manual) */
        "kismet_rwwait%=:\n\t"
        "SKPDN DSKP\n\t"           /* controller Busy/Done flag -- standard
                                    *   generic Nova/Eclipse polled
                                    *   completion, same idiom disk_probe.s
                                    *   used for DSK */
        "JMP kismet_rwwait%=\n\t"
        "DIA 0,DSKP\n\t"           /* bare: read final status, don't clear */
        "ESTA 0,_k_status,0\n\t"
    );
}

/* Shared by kismet_read_block/kismet_write_block -- see kismet.h for
 * the full contract (return value convention, CHS division). */
static int kismet_rw_op(int unit, int heads, unsigned int blockno,
                         void *buf, int cmd) {
    unsigned int sectors_per_cyl = (unsigned int)heads * KISMET_SECTORS_PER_TRACK;
    unsigned int cyl = blockno / sectors_per_cyl;
    unsigned int rem = blockno % sectors_per_cyl;
    unsigned int head = rem / KISMET_SECTORS_PER_TRACK;
    unsigned int sector = rem % KISMET_SECTORS_PER_TRACK;

    kismet_select_and_check(unit);
    if (!(_k_dib_status & KISMET_DIB_READY) ||
        (_k_dib_status & KISMET_DIB_DRV_FAULT)) {
        return -1; /* not ready / faulted -- see kismet.h's return-value doc */
    }

    kismet_seek_and_xfer(unit, cmd, (int)cyl, (int)head, (int)sector, buf);
    return (int)_k_status;
}

int kismet_read_block(int unit, int heads, unsigned int blockno, void *buf) {
    return kismet_rw_op(unit, heads, blockno, buf, KISMET_CMD_READ);
}

int kismet_write_block(int unit, int heads, unsigned int blockno, void *buf) {
    return kismet_rw_op(unit, heads, blockno, buf, KISMET_CMD_WRITE);
}
