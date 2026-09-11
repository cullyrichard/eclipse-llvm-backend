#ifndef _MMPU_H
#define _MMPU_H

/* C-callable wrappers around the Eclipse S/140 MMPU's verified
 * single-cycle mapping mechanism (see MMPU_NOTES.md's Phase 1 section,
 * and examples/mmpu_probe.s, which this is a parameterized, reusable
 * version of). Entirely supervisor-mode -- no user-mode context
 * switch, no page-fault handling, same explicit scope Phase 1 had.
 *
 * physpage: physical page number (0-1023, 10 bits -- see MMPU_NOTES.md
 *   for why 1024, not 512 or 2048).
 * offset: word offset within that physical page (0-1023).
 * Together they name any word in the S/140's real 1-megaword physical
 * address space, including everything past the 32768-word logical
 * ceiling every *ordinary* compiled access (ELDA/ESTA/EJSR/ELEF, and
 * therefore every normal C pointer) is confined to.
 */
int mmpu_read_far(unsigned int physpage, unsigned int offset);
void mmpu_write_far(unsigned int physpage, unsigned int offset, unsigned int value);

#endif
