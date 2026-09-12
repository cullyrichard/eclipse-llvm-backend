// kismet_probe.s -- hand-assembled, register-level walkthrough of one
// write-then-readback round trip against the DG-Disk Storage Subsystem
// ("Kismet", Models 6160/6161/6214, device mnemonic DSKP), unit 0,
// cylinder 0, head 0, sector 0 (the driver's linear block 0).
//
// IMPORTANT, unlike examples/disk_probe.s or examples/mmpu_probe.s:
// this file has been assembled (dgasm accepts every mnemonic below --
// confirmed directly, see KISMET_NOTES.md's verification section) but
// has NOT been run, single-stepped, or otherwise executed against any
// simulator or real hardware. This project's SIMH build does not
// model Kismet/DSKP at all, so a `run` here would either do nothing
// (device code 027 unattached -- SKPDN would never see Done, hanging
// forever in the wait loop) or, on real Eclipse hardware, would be a
// genuine but UNVERIFIED first live test. Do not treat this file's
// existence as proof the sequence is correct -- see KISMET_NOTES.md
// for exactly what is and isn't established.
//
// Register model and every field cited below: Programmer's Reference
// rev 1 (Data General pub. 014-000654-01, June 1982), as noted inline.
// Assemble (this step IS verified, against the real dgasm binary):
//   dgasm -t eclipse_s140 -f simh -o kismet_probe.simh kismet_probe.s

	org 050

_start:
	// --- Phase I (p.11): select drive 0, load the SEEK command,
	// then read drive status back before doing anything else. DOA
	// bits: 0=Clear R/W Done(0), 1-2=Clear Seek Done(0,0), 3-4=not
	// used, 5-8=Command (0010 = Seek, p.5's command table), 9=must
	// be 0, 10=Drive(0), 11-15=Extended Memory Address MSBs(0) --
	// full field layout p.5.
	LDA 0, doa_seek
	DOA 0, DSKP              // bare -- no pulse yet

	DIB 0, DSKP               // bare: read drive status (p.9) --
	                           // does not clear anything (assumed,
	                           // not confirmed against source --
	                           // see KISMET_NOTES.md)
	ESTA 0, dib_status, 0

	// --- Phase II (p.11): position the heads. DOC "Specify
	// Cylinder" (p.6) is only valid in this context because the
	// prior DOA specified a SEEK. Cylinder 0 here. f=P starts the
	// seek and, per the manual's own text, "does not affect the
	// Busy flag or Done flag" (p.11) -- so there is nothing to poll
	// here; Phase III can proceed immediately (p.11: "If a read/
	// write operation is to follow, proceed immediately to Phase
	// III without waiting for a drive attention interrupt
	// request").
	LDA 0, doc_cyl
	DOCP 0, DSKP              // P pulse: starts the SEEK

	// --- Phase III (p.11-12): select drive 0 again, this time with
	// the WRITE command (1110 binary, p.5's command table).
	LDA 0, doa_write
	DOA 0, DSKP                // bare

	// --- Phase IV (p.12): extended sector/count, then head/sector/
	// count, then memory address, then start.
	//
	// "Specify Extended, Sector and Count" (1st DOC, p.6-7): bit 4
	// = HD MSB (0, head 0 needs no extra bit), bit 5 = Sector
	// Address MSB (0, sector 0), bit 10 = Sector Count MSB. Count
	// is a two's complement of the sector count in a combined
	// 6-bit field (bit 10 here + bits 11-15 of the 2nd DOC below);
	// for a 1-sector transfer, 64-1=63 decimal=077 octal=all six
	// bits set, so bit 10 here = 1.
	LDA 0, doc1_extsec
	DOC 0, DSKP                 // bare

	// "Specify Head, Sector and Count" (2nd DOC, p.7): bit 0 = MAP
	// (0, not using BMC mapped addressing), bits 1-5 = Head Address
	// (0), bits 6-10 = Sector Address (0), bits 11-15 = Sector
	// Count low 5 bits (037 octal, all five bits set -- see above).
	LDA 0, doc2_headsec
	DOC 0, DSKP                  // bare

	// "Specify Memory Address" (DOB, p.8): bit 0 = EMA LSB (0, no
	// extended/BMC-mapped addressing here -- plain 15-bit logical
	// address, same convention examples/disk_probe.s and
	// examples/mmpu.c already use), bits 1-15 = the buffer address.
	// f=S here both loads the register AND starts the transfer
	// (p.11's "S" bullet: sets Busy, clears Done, "Starts the
	// following operations. READ, WRITE, FORMAT, VERIFY, or READ
	// BUFFERS").
	ELEF 1, wbuf                   // AC1 = address of wbuf (not its
	                                //   value) -- same ELEF-then-DOx
	                                //   idiom disk_probe.s uses for DSK
	DOBS 1, DSKP                   // S pulse: sets mem addr = AC1, starts WRITE

wwait:
	SKPDN DSKP                     // controller Busy/Done flag --
	JMP wwait                      //   standard polled completion,
	                                //   same idiom as disk_probe.s's
	                                //   DSK usage
	DIA 2, DSKP                    // bare: read final status
	ESTA 2, wstatus, 0

	// --- Read phase: same CHS target (unit 0, cyl 0, head 0,
	// sector 0), READ command (0000 binary) this time, reading into
	// a separate, pre-zeroed buffer `rbuf`.
	LDA 0, doa_seek2
	DOA 0, DSKP
	DIB 0, DSKP
	ESTA 0, dib_status2, 0

	LDA 0, doc_cyl
	DOCP 0, DSKP

	LDA 0, doa_read
	DOA 0, DSKP

	LDA 0, doc1_extsec
	DOC 0, DSKP

	LDA 0, doc2_headsec
	DOC 0, DSKP

	ELEF 1, rbuf                   // AC1 = address of rbuf
	DOBS 1, DSKP                   // S pulse: sets mem addr = AC1, starts READ

rwait:
	SKPDN DSKP
	JMP rwait
	DIA 2, DSKP
	ESTA 2, rstatus, 0

	HALT

	org 0200
	dev DSKP = 027

// --- Phase I/III register words (unit 0 throughout; see field
// layouts cited above each phase) ---
doa_seek:
	dw 0000400                     // DOA: drive 0, cmd=0010b (Seek) = 2
	                                //   decimal, in bits 5-8 (shift left
	                                //   7) = 256 decimal = 0400 octal
doa_seek2:
	dw 0000400                     // same, reissued before the read phase
doa_write:
	dw 0003400                     // DOA: drive 0, cmd=1110b (Write) = 14
	                                //   decimal (016 octal), shift left 7
	                                //   = 1792 decimal = 03400 octal
doa_read:
	dw 0000000                     // DOA: drive 0, cmd=0000b (Read) = 0

doc_cyl:
	dw 0000000                     // DOC "Specify Cylinder": cylinder 0

doc1_extsec:
	dw 0000040                     // DOC "1st": bit 10 (count MSB) = 1
	                                //   -> 1 << 5 = 040 octal
doc2_headsec:
	dw 0000037                     // DOC "2nd": head=0,sector=0,
	                                //   count low5 = 037 (all set)

dib_status:
	dw 0
dib_status2:
	dw 0
wstatus:
	dw 0
rstatus:
	dw 0

// write buffer: 256 words (one real 512-byte sector, p.3), word 0/1
// carry distinct markers, word 255 a third -- same convention
// disk_probe.s used to prove a full-sector transfer, not just its
// first words.
wbuf:
	dw 0123456, 0000377
	var wbuf_mid resv 0375
	dw 0177777

var rbuf resv 0400
