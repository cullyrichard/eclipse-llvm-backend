// disk_probe.s -- minimal proof that a program *running on the simulated
// Eclipse CPU* (not SIMH's own `attach`/`examine` commands touching the
// image file directly) can drive the DSK device: attach a disk image,
// write one full 256-word sector from memory to block 5, then read that
// same block back into a *different* memory buffer and prove the words
// round-tripped byte-for-byte through the device.
//
// Device chosen: DSK (device code 020 octal), the "4019 fixed head disk"
// per nova_dsk.c's own header comment (`dsk  fixed head disk` / `The 4019
// is a head-per-track disk`) -- a real DG part number given directly by
// the simulator author, not inferred. See STORAGE_NOTES.md at the repo
// root for the full DSK-vs-DKP comparison and why DSK was picked over
// DKP (the 4-unit moving-head "disk pack" device, nova_dkp.c) for this
// first probe.
//
// DSK's register model (all confirmed directly against nova_dsk.c, not
// guessed):
//   DOA <AC>, DSK   (no pulse) -- sets dsk_da, the block address
//                      (linear: block*256 must stay under the unit's
//                      word capacity -- 262144 words / 256 = 1024
//                      blocks for the default 1-platter size `show dsk`
//                      reports as "262KW").
//   DOB <AC>, DSK   (no pulse) -- sets dsk_ma, the starting memory
//                      address for the transfer.
//   NIOP DSK        -- pulse P: starts a WRITE. dsk_svc's unit-service
//                      routine (scheduled after a fixed per-sector
//                      delay -- dsk_time, simulating rotational
//                      latency) copies 256 words from M[dsk_ma..] into
//                      the attached file's in-memory buffer at the
//                      block's offset. This is simulated cycle-stealing
//                      DMA, not word-by-word PIO -- the CPU issues one
//                      trigger and the whole sector moves without
//                      further CPU involvement (see dsk_svc's `for (i =
//                      0; i < DSK_NUMWD; i++)` loop in nova_dsk.c).
//   NIOS DSK        -- pulse S: starts a READ (same transfer, reverse
//                      direction).
//   SKPDN DSK       -- skip next instruction if the device's Done flag
//                      is set. Completion here is a plain polled
//                      Busy/Done pair (DEV_SET_BUSY/DEV_SET_DONE +
//                      DEV_UPDATE_INTR, same generic mechanism TTI/TTO
//                      use) -- interrupt-driven completion is also
//                      possible (INT_DSK/PI_DSK are wired the same way)
//                      but not exercised here; this probe polls.
//   DIA <AC>, DSK   (no pulse, does NOT clear status) -- reads dsk_stat
//                      masked to the error-flag bits (DSKS_ALLERR =
//                      write-lock/data-late/nonexistent-disk/CRC/error-
//                      summary). 0 = no error.
//
// Assemble: dgasm -t eclipse_s140 -f simh -o disk_probe.simh disk_probe.s
// Run (see STORAGE_NOTES.md for the exact captured transcript):
//   $ rm -f disk_probe.img
//   $ { echo 'attach dsk disk_probe.img'; cat disk_probe.simh; \
//       echo 'dep PC 50'; echo 'step 200'; \
//       echo 'e wstatus'; echo 'e rstatus'; \
//       echo 'e rbuf'; echo 'e rbuf+1'; echo 'e rbuf+377'; \
//       echo 'quit'; } | eclipse

	org 050

_start:
	// --- write phase: wbuf (256 words, marked at word 0/1/255) -> block 5 ---
	LDA 0, blockno
	DOA 0, DSK              // dsk_da = 5

	ELEF 1, wbuf             // AC1 = address of wbuf
	DOB 1, DSK               // dsk_ma = &wbuf

	NIOP DSK                 // pulse P: start WRITE (wbuf -> disk block 5)

wwait:
	SKPDN DSK
	JMP wwait

	DIA 2, DSK               // AC2 = write status (error flags), not cleared
	ESTA 2, wstatus          // extended STA: wstatus is past the plain
	                          // 0-255 direct-page addressing range

	// --- read phase: block 5 -> rbuf (separate, pre-zeroed buffer) ---
	LDA 0, blockno
	DOA 0, DSK               // dsk_da = 5 (again -- DOA doesn't pulse, so
	                          // this is a fresh, explicit register set,
	                          // not relying on the write phase's leftover
	                          // value)

	ELEF 1, rbuf              // AC1 = address of rbuf
	DOB 1, DSK                // dsk_ma = &rbuf

	NIOS DSK                  // pulse S: start READ (disk block 5 -> rbuf)

rwait:
	SKPDN DSK
	JMP rwait

	DIA 2, DSK                // AC2 = read status
	ESTA 2, rstatus            // extended STA, same reason as above

	HALT

	org 0100
	dev DSK = 020

blockno:
	dw 5

// write buffer: 256 words total. Word 0 and word 1 carry distinct
// marker values; the remaining 253 words are reserved (zero-filled by
// dgasm's `resv`); the final word (index 255) carries a third marker,
// to prove the *entire* sector transferred, not just its first words.
wbuf:
	dw 0123456, 0000377
	var wbuf_mid resv 0375
	dw 0177777

// read buffer: 256 words, all zero, to be overwritten by the READ.
var rbuf resv 0400

wstatus:
	dw 0
rstatus:
	dw 0
