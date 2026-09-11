#include "mmpu.h"

/* File-scope (module-level) asm, not inside any function: always
 * emitted regardless of which of mmpu_read_far/mmpu_write_far actually
 * survive this program's own dead-code elimination (see eclipse-cc's
 * "Only main needs to survive" comment) -- confirmed empirically that
 * this is emitted unconditionally, unlike a `dev` declaration placed
 * inside a function body's own asm block, which only survives if that
 * specific function does. A duplicate `dev MAP = 03` (e.g. one copy
 * per function, if both happen to survive) is a hard dgasm error
 * ("Multiple definitions for symbol MAP"), confirmed empirically --
 * this is why there is exactly one declaration, here, not one per
 * function. */
asm("dev MAP = 03");

/* Internal state, deliberately round-tripped through named globals
 * rather than kept in registers across the asm block via operand
 * substitution (contrast examples/fps.h's fpu_out/fpu_in, which *do*
 * use "r"-constrained operands for DOA/DOB's single ac,device
 * operand). That works for fps.h because DOA/DOB's accumulator is a
 * genuine operand slot the compiler's register allocator can fill
 * with anything. LMP is different: it hard-codes AC0 (relocation),
 * AC1 (count), and AC2 (source address) by *architectural convention*,
 * not by operand encoding -- dgasm's bare `LMP` takes zero operands at
 * all (see examples/mmpu_probe.s). Mixing "r"-constrained operands
 * (which the register allocator could place in AC0-AC2 for an
 * unrelated reason) with hard-coded AC0/AC1/AC2 clobbers in the same
 * asm block risks a collision this backend's inline-asm implementation
 * has no verified way to prevent (no clobber-list or fixed-register
 * constraint support has been confirmed to exist). Routing every value
 * through memory (ELDA/ESTA to a global, referenced by its literal,
 * unmangled C name -- confirmed empirically not to be name-mangled)
 * sidesteps the whole question: nothing needs to survive in a register
 * across the block. */
static unsigned int _mmpu_pte;
static unsigned int _mmpu_target;
static unsigned int _mmpu_value;
/* AC2/AC3 save slots -- see the header comment on _mmpu_pte above for
 * why every value crosses the asm boundary through memory, not
 * registers. AC2 turned out to need the same treatment for a *different*
 * reason: it's not just an operand-substitution risk, it's this
 * backend's live frame pointer for the whole function body (every
 * frame-relative access, e.g. `LDA 0,-6,2`, uses it) -- confirmed by
 * single-stepping a first version of this file that clobbered AC2 via
 * ELEF/ELDA without restoring it: the compiler's own post-asm epilogue
 * (storing the return value to a frame-relative stack slot right
 * before RTN) silently used the *clobbered* AC2 as its base address,
 * so the real result never reached the slot RTN's caller-side epilogue
 * reads from -- the wrong value observed (in that broken version) was
 * simply whatever AC0 held at the call site before SAVE pushed it,
 * restored verbatim by RTN, completely unrelated to this function's
 * own computation. AC3 isn't confirmed live across this specific
 * function's asm block the same way, but is saved/restored anyway --
 * cheap insurance against relying on that not mattering here. */
static unsigned int _mmpu_save2;
static unsigned int _mmpu_save3;

int mmpu_read_far(unsigned int physpage, unsigned int offset) {
    /* PTE word format (MMPU_NOTES.md, LMP's own bit table, bit-for-bit
     * verified against the real manual): bit 0 = write protect (left 0
     * here, matching mmpu_probe.s's already-verified value -- see that
     * file's own header comment on the write-protect bit's ambiguous
     * manual wording, not re-litigated here), bits 1-5 = logical page
     * (left 0 -- logical page 0, matching _mmpu_target's own page),
     * bits 6-15 = physical page number. mmpu_probe.s's `pte: dw 050`
     * is the same encoding with physpage baked in at assemble time
     * instead of passed at runtime. */
    _mmpu_pte = physpage & 01777;
    /* Redirect target: logical address `offset` within logical page 0
     * -- since the PTE above maps logical page 0's slot, and the
     * single-cycle mechanism only remaps *one* logical page, `offset`
     * doubles as both "which word" and "which logical address to
     * touch" (see mmpu.h's own comment: offset is 0-1023, exactly the
     * in-page range one PTE covers). */
    _mmpu_target = offset & 01777;
    /* Dummy writes -- with no plain-C reference at all, globaldce
     * removes _mmpu_save2/_mmpu_save3's storage entirely (confirmed
     * empirically: "Undefined symbol: _mmpu_save2" at the dgasm step),
     * since every *real* reference lives only in the invisible asm
     * text below. The values written here don't matter -- the asm
     * block's own first two instructions overwrite them immediately. */
    _mmpu_save2 = 0;
    _mmpu_save3 = 0;
    asm volatile(
        "ESTA 2,_mmpu_save2,0\n\t" /* save the live frame pointer (AC2)
                                    *   and AC3 before clobbering either */
        "ESTA 3,_mmpu_save3,0\n\t"
        "SUB 0,0\n\t"              /* AC0 = 0 (DOA MAP's MapStat value: */
        "DOA 0,MAP\n\t"            /*   map-select=User A, Enable=User A) */
        "SUB 1,1\n\t"              /* AC1 = 0, then */
        "ADI 1,1\n\t"              /*   +1 -> AC1 = 1 (LMP word count) */
        "ELEF 2,_mmpu_pte,0\n\t"   /* AC2 = address of _mmpu_pte (not its
                                    *   value -- ELEF, not ELDA) */
        "LMP\n\t"                  /* Map[UserA][0] = _mmpu_pte's value */
        "ELDA 2,_mmpu_target,0\n\t" /* AC2 = offset (the redirect target
                                    *   address itself, now that LMP no
                                    *   longer needs AC2 for anything) */
        "NIOP MAP\n\t"             /* arm single-cycle using the last
                                    *   user map (User A, per DOA above) */
        "ELDA 3,0,2\n\t"           /* AC3 = *(AC2) -- the one mapped
                                    *   access; SingleCycle auto-reverts
                                    *   immediately after */
        "ESTA 3,_mmpu_value,0\n\t" /* _mmpu_value = AC3, so C can read it
                                    *   back (this ESTA itself is a
                                    *   normal, unmapped access -- only
                                    *   the ELDA right before it was
                                    *   redirected) */
        "ELDA 2,_mmpu_save2,0\n\t" /* restore AC2/AC3 before falling back
                                    *   into compiler-generated code that
                                    *   assumes AC2 is still the frame
                                    *   pointer */
        "ELDA 3,_mmpu_save3,0\n\t"
    );
    return (int)_mmpu_value;
}

void mmpu_write_far(unsigned int physpage, unsigned int offset, unsigned int value) {
    _mmpu_pte = physpage & 01777;
    _mmpu_target = offset & 01777;
    _mmpu_value = value;
    _mmpu_save2 = 0; /* dummy writes -- see mmpu_read_far's comment */
    _mmpu_save3 = 0;
    asm volatile(
        "ESTA 2,_mmpu_save2,0\n\t" /* save AC2 (frame pointer) / AC3 --
                                    *   see mmpu_read_far's comment */
        "ESTA 3,_mmpu_save3,0\n\t"
        "SUB 0,0\n\t"
        "DOA 0,MAP\n\t"
        "SUB 1,1\n\t"
        "ADI 1,1\n\t"
        "ELEF 2,_mmpu_pte,0\n\t"
        "LMP\n\t"
        "ELDA 2,_mmpu_target,0\n\t"
        "ELDA 3,_mmpu_value,0\n\t" /* AC3 = the caller's value, loaded
                                    *   *before* arming single-cycle --
                                    *   this ELDA is a normal, unmapped
                                    *   access */
        "NIOP MAP\n\t"
        "ESTA 3,0,2\n\t"           /* *(AC2) = AC3 -- the one mapped
                                    *   write; SingleCycle auto-reverts */
        "ELDA 2,_mmpu_save2,0\n\t" /* restore AC2/AC3 before returning to
                                    *   compiler-generated code */
        "ELDA 3,_mmpu_save3,0\n\t"
    );
}
