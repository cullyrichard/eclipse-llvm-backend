# DSKP family unification — one shared core, three generation variants

Three independent scoping efforts on this codebase each built a block
driver for a different real DG disk-subsystem generation the user owns:
Zebra (DG 6060/6061/6067, 1976-1980, `ZEBRA_NOTES.md`), Vulcan (DG 6122,
1979-1980, `VULCAN_NOTES.md`), and Kismet (DG 6160/6161/6214, 1981-1982,
`KISMET_NOTES.md`). All three, working from three physically separate
manuals, independently discovered the identical `027` octal device
select code and `DSKP` register convention — Data General reused one
standing "DG/Disc Storage Subsystem" programming model across a decade
of hardware generations rather than inventing a new one each time.
`KISMET_NOTES.md`'s own "A real cross-driver conflict" section already
documented this as a real, reproduced build failure (two independent
`dev DSKP = 027` declarations assembling together: `Multiple
definitions for symbol DSKP.`). This file documents unifying the three
drivers onto one shared register core (`examples/dskp_common.h/.c`)
with a compile-time variant selector, so exactly one generation's driver
is ever linked into a given build, and the collision is resolved by
construction rather than by discipline alone.

**Bottom line, up front:** unification is done, the original symbol
collision is fixed and the fix is verified empirically (not just
argued), and no generation-specific behavior was flattened in the
process — each of Zebra's burst/FIFO/ECC-capability scope note,
Vulcan's split sector/count MSB fields, and Kismet's context-sensitive
DOC phase and its two documented manual inconsistencies survive the
refactor, cited below against exactly where they now live. The
pre-existing caveat all three drivers already carried is unchanged:
**none of Zebra/Vulcan/Kismet has ever executed against real hardware
or a simulator** (SIMH doesn't model any of the three) — compile/
assemble-clean through the real `eclipse-cc`/`dgasm` pipeline remains
the verification ceiling, exactly as before. Unifying the three onto a
shared core does not, and could not, change that.

## What's actually shared (found by reading all three drivers directly,
not guessed at from the three NOTES files' prose)

Reading `examples/zebra.c`, `examples/vulcan.c`, and `examples/
kismet.c` side by side (as they stood before this refactor — see `git
log` for their pre-unification content) turned up a real, concrete
common substrate, not just "they're all DSKP":

- **Device identity.** All three: select code `027` octal (alt `067`),
  mnemonic `DSKP`, priority mask bit 7. Identical in all three manuals.
- **The 16-entry command table, DOA bits 5-8.** Byte-for-byte identical
  bit position and command assignment in Zebra's and Vulcan's own
  manuals (`READ`=0 through `FORMAT`=17 octal); Kismet's own manual
  marks command codes `0011`-`1000` binary "Reserved" rather than
  individually confirming STOP/OFFSET FWD/OFFSET REV/WRITE DISABLE/
  RELEASE/TRESPASS by name the way Zebra/Vulcan's manuals do, but every
  value Kismet's manual *does* name (READ/RECAL/SEEK, ALT MODE 1/2,
  NOP, VERIFY, READ BUFFERS, WRITE, FORMAT) matches exactly. Flagged
  in `dskp_common.h`'s own comment, not silently assumed confirmed for
  all 16 entries in all three sources.
- **DOA bit 0, "Clear R/W Done".** Set unconditionally by all three
  drivers already, before this refactor. Vulcan's and Kismet's own
  manuals state this bit's meaning directly; Zebra's manual leaves the
  bit unlabeled beyond "R/W" (an inference by analogy, already flagged
  in `ZEBRA_NOTES.md` — carried forward unchanged, not strengthened by
  unification).
- **DIA fault-bit field, bits 6-15 — the single strongest piece of
  evidence for a real shared substrate.** Transcribed independently by
  all three original scoping efforts, and cross-checked line-by-line
  against each other while building this file: PARITY(6), INVALID/
  ILLEGAL SECTOR(7), ECC(8), BAD SECTOR(9), CYLINDER ERROR(10),
  SEC/HEAD ERROR(11), VERIFY ERROR(12), R/W TIMEOUT(13), DATA LATE(14),
  R/W FAULT(15) — identical bit position AND meaning in Zebra's,
  Vulcan's, and Kismet's manuals, down to the bit-11 case where the
  three generations use three different English words for the
  identical bit and fault condition ("SEC/HD ERR" Zebra, "Surf/sect
  error" Vulcan, "Head/sect error" Kismet — same bit, same fault,
  different generation's own terminology for the surface-or-head
  addressing axis). Ten bits, three independently-read manuals, exact
  agreement — this is the finding that makes "one shared register
  convention across three hardware generations" more than a device-code
  coincidence.
- **DIB status bits — five positions confirmed identical in all
  three:** Ready(3), Busy(4), Write Disabled(6), Positioner Fault(12),
  Drive Fault(15). Zebra's and Vulcan's manuals additionally agree with
  *each other* (not shared with Kismet's manual, which simply doesn't
  document these bits either way) on Invalid Status(0)/Reserved(1)/
  Trespassed(2)/Offset(5)/Invalid Address(8)/Illegal Command(9)/Power
  Fault(10)/Pack Unsafe(11)/Clock Fault(13)/Write Fault(14) — a real
  two-generation cross-check kept as Zebra/Vulcan-local, not promoted
  into the shared core, since Kismet's own manual doesn't confirm those
  bits at all (an honest gap in that manual's own documentation, not
  something this project can fill in by assuming Kismet agrees).
  Vulcan's manual alone additionally documents a bit-7 "Drive ID" field
  neither Zebra's nor Kismet's manual has.
- **DOB "Specify Memory Address": bit0 = EMA LSB (unused by any of the
  three drivers), bits1-15 = address.** Identical in all three.
- **DOC context-sensitivity on the prior DOA.** All three manuals —
  not just Kismet's — document that DOC's meaning depends on whether
  the immediately preceding DOA specified SEEK ("Specify Cylinder", 10
  bits at bits 6-15, identical field position in all three) or
  something else ("Specify Surface/Head, Sector and Count", shape
  varies — see "What's genuinely different" below). The task brief that
  started this unification effort described this context-sensitivity as
  a Kismet-specific property; reading Zebra's and Vulcan's own manuals
  directly (Appendix A p.A-1's "IF PREVIOUS DOA SPECIFIED SEEK" vs.
  not, and pgmref.pdf p.5-6's identical framing) shows it is not —
  it's the shared shape of the whole DSKP family's DOC register, and
  `dskp_common.h`'s own comments say so plainly rather than silently
  going along with the framing.
- **S/C/P flag-command semantics.** Word-for-word the same substance in
  all three manuals (quoted in full in `dskp_common.h`): f=S starts
  READ/WRITE/FORMAT/READ BUFFERS/VERIFY and sets Busy/clears Done; f=C
  stops everything and clears Busy/Done; f=P starts SEEK/RECAL/OFFSET/
  STOP/WRITE DISABLE/RELEASE/TRESPASS and does **not** touch Busy/Done
  — the real, family-wide consequence being that a P-pulsed SEEK is
  never visible to a `SKPDN`/`SKPBN` poll in any of the three
  generations.
- **Register-load instruction sequences** (bare DOA/DOC/DOB, bare
  DIA/DIB reads, combined `DOCP` seek-trigger, combined `DOBS`+
  poll-to-completion transfer-trigger) were, before this refactor,
  three separate, nearly line-for-line identical implementations —
  `zebra.c`'s `dskp_doa`/`dskp_doc`/`dskp_dia`/`dskp_dib`/
  `dskp_docp_seek`/`dskp_dobs_start` and `vulcan.c`'s functions of the
  same names were already byte-for-byte identical in behavior (differing
  only in that both pasted the literal text `"027"` into every asm
  string rather than referencing a symbolic device name); `kismet.c`'s
  equivalent logic did the same work through a different, global-
  variable-plus-`ELDA`/`ESTA` mechanism. This is the most direct,
  concrete duplication the unification removes.

## What's genuinely different (real, preserved, not flattened)

### Zebra — the richest of the three, real extra capability beyond the shared core

- **DOA layout:** 2-bit drive-select field (bits 9-10, drives 0-3) and
  a 4-bit "clear attention" field (bits 1-4, one bit per drive) — same
  field widths as Vulcan's, different from Kismet's (see below).
- **The "not-SEEK" DOC is a single word.** Surface (5 bits, 0-18),
  sector (5 bits, 0-23), and count (5 bits, two's complement) all fit
  directly — Zebra's narrower geometry (24 sectors/track, 32-sector max
  burst) never needs the MSB-split trick Vulcan/Kismet require. This
  stays a genuinely single-DOC code path in `zebra.c`, not forced into
  a shared two-DOC shape.
- **Command-sequencing strategy: explicit per-drive attention-bit
  poll.** `zebra_seek()` issues the P-pulsed seek, then explicitly polls
  *this drive's own* DIA attention bit (bits 2-5, Zebra-specific, not
  in the shared core since Vulcan/Kismet number this differently or
  don't use it) until it sets, then clears it — the conservative choice
  `ZEBRA_NOTES.md` already explained (Appendix A's flowcharts are
  diagrams only, with no prose confirming what "CONTROL BUSY"/"CONTROL
  FULL" individually gate, so this driver doesn't take the flowchart's
  own documented shortcut). This is the one real place all three
  generations' command SEQUENCING (not just register layout) diverges:
  Zebra waits explicitly; Vulcan and Kismet both rely on the
  controller's own documented internal seek-deferral and never poll for
  it at all. Kept entirely in `zebra.c` — `dskp_common.c` has no
  seek-completion-detection logic of any kind, deliberately, since the
  three generations don't agree on one.
- **Zebra's own extra DIB fault bits** (Invalid Status, Reserved,
  Trespassed, Positioner Offset, Invalid Address, Illegal Command,
  Power Fault, Pack Unsafe, Clock Fault, Write Fault) — on top of the
  five shared DIB bits — are unchanged, still in `zebra.c`.
- **Real extra capability beyond anything the shared core or driver
  implements, unchanged from before unification:** explicit multi-
  sector burst count (up to 32 sectors/operation), an 8-word FIFO
  buffer, 32-bit ECC readback via the `SET ALT MODE 1`/`SET ALT MODE 2`
  commands (`DSKP_CMD_ALT_MODE1`/`_ALT_MODE2`, now shared family
  constants — the entry points a future increment would use), and
  per-drive RESERVE/TRESPASS. None of this was implemented before
  unification and none of it is implemented now — `zebra.h`'s own scope
  comment says so, unchanged.

### Vulcan — the split sector/count MSB fields

- **DOA layout:** same 2-bit drive-select (bits 9-10) / 4-bit clear-
  attention (bits 1-4) field widths as Zebra's.
- **The "not-SEEK" case needs TWO DOCs.** A first "Specify Extended
  Sector and Count" DOC carries the sector-address MSB (bit 5) and
  sector-count MSB (bit 10); a second "Specify Surface, Sector and
  Count" DOC carries MAP/surface(5b)/sector-low-5/count-low-5 — because
  35 sectors/track and a 64-sector max count don't fit in Zebra's
  narrower 5-bit fields. This is exactly the "sector/sector-count
  fields split 1 MSB + 5 LSB across two separate `DOC` writes"
  subtlety `VULCAN_NOTES.md` flags as a real, easy-to-get-wrong detail
  — preserved bit-for-bit in `vulcan.c`'s `vulcan_rw()`, unchanged by
  this refactor (`doc1_word`/`doc2_word`'s construction is textually
  identical to the pre-refactor version, just calling the shared
  `dskp_doc()` instead of a file-local copy of the same function).
- **Command-sequencing strategy: fire-and-forget**, relying on the
  manual's own documented guarantee that the controller defers the
  stored read/write command internally until the outstanding seek
  finishes — no attention-bit poll, unlike Zebra. Same strategy
  Kismet's driver uses (see below); Vulcan and Kismet share this,
  Zebra alone differs.
- **DIA bit 0 = Control Full** — a real, Vulcan-specific bit
  *position* (`VULCAN_DIA_CONTROL_FULL`, defined locally in
  `vulcan.c`, not in the shared core): Zebra's manual places the
  analogous bit at bit 1 instead (an inferred name, per
  `ZEBRA_NOTES.md`), so this bit could not be promoted into
  `dskp_common.h`'s shared constants without erasing that real
  disagreement between the manuals.

### Kismet — the context-sensitive DOC phase, and two carried-forward inconsistencies

- **DOA layout is narrower than the other two: 1-bit drive-select**
  (bit 10 only, drives 0-1) with bit 9 required to be 0, and a **2-bit**
  "Clear Seek Done" field (bits 1-2, drives 0-1) — tracking Kismet's
  real 2-drives-per-controller limit (`KISMET_UNIT_0`/`_1`), vs.
  Zebra's/Vulcan's 4-drive, 2-bit fields. Kept in `kismet.c`, not
  forced into the shared core's (nonexistent) generic drive-select
  helper — `dskp_common.h` deliberately does not provide one, since the
  three generations' field widths don't agree.
- **The "not-SEEK" case needs TWO DOCs, same shape as Vulcan's**, with
  one real extra field Vulcan's doesn't have: the first DOC also
  carries a **Head Address MSB (bit 4)**, needed because the 6214's 40
  heads don't fit in the base 5-bit head field (Vulcan's fixed
  19-surface geometry never needs a surface-address MSB at all). This
  field-level difference — same two-DOC *shape* as Vulcan, one extra
  bit Vulcan doesn't need — is exactly the kind of detail a naive
  "just merge the two-DOC drivers" refactor could have silently lost;
  it's preserved explicitly in `kismet.c`'s `kismet_seek_and_xfer()`
  (`head_msb`, shifted into DOC1 bit 4) and called out in this file.
- **Command-sequencing strategy: fire-and-forget**, same as Vulcan's,
  and in fact the most *explicit* of the three manuals about it:
  "If a read/write operation is to follow, proceed immediately to
  Phase III without waiting for a drive attention interrupt request"
  (Programmer's Reference rev 1, p.11-12, quoted in full in
  `KISMET_NOTES.md`).
- **Two genuine internal manual inconsistencies, carried forward
  unresolved, exactly as `KISMET_NOTES.md` already documented them —
  not silently resolved differently by this refactor:**
  1. The DOC/DIC "Specify Head, Sector and Count" register's own bit
     table lists bit 1 *twice* — once as a standalone "Reserved" row,
     once as the low end of the "1-5 Head Address" range. The
     field-boundary diagram (not the prose table) is unambiguous, and
     that's what `kismet.c`'s bit math has always used (`(head & 037u)
     << 10`, a genuine 5-bit field at bits 1-5) — unchanged by this
     refactor.
  2. DIB alternate-mode-1's prose claims an "extended head count in bit
     4" that the same page's own bit table marks "Reserved". Not
     exercised by this driver either before or after unification (the
     driver never reads DIB alternate mode 1 at all).
  Both are recorded here, again, on purpose — per the task's own
  instruction not to let a unification pass quietly pick a different
  resolution than the original investigation already picked, or worse,
  paper over the fact that the manual disagrees with itself.

## The variant-selection mechanism

Each variant's own `.c` file selects its generation with a single
`#define` before including the shared core:

```c
#define DSKP_VARIANT_ZEBRA      /* or _VULCAN, or _KISMET */
#include "dskp_common.h"
#include "zebra.h"              /* or vulcan.h, or kismet.h */
```

`dskp_common.h` requires **exactly one** of `DSKP_VARIANT_ZEBRA`/
`_VULCAN`/`_KISMET` to be defined in the including translation unit, or
it fails to compile with a `#error` naming the requirement directly —
this catches, within one `.c` file, a mistake like accidentally
`#include`-ing two of `zebra.h`/`vulcan.h`/`kismet.h`'s implementation
files together. `examples/dskp_common.c` itself — the shared core's own
implementation, deliberately variant-agnostic (every function it
defines behaves identically regardless of which generation is selected)
— opts out of this requirement with its own `DSKP_COMMON_CORE_TU`
guard, since it has no way to know, and no need to know, which variant
it'll eventually be linked alongside.

### Why a header-level `#error` alone can't catch everything, and what
### does catch the rest

This project's build tool, `eclipse-cc`, has no `-D` command-line
flag and dgasm has no separate-compilation/linking model of its own
(confirmed by reading `eclipse-cc`'s own script and header comment
directly — every `.c` file it's given is compiled independently via
`clang -cc1`, then all of their LLVM IR is merged into one module via
`llvm-link` *before* dgasm ever runs). That means there is no
preprocessor-visible signal one translation unit (say, `zebra.c`) could
check against another's choice (say, `vulcan.c`) — the `#error` guard
above can only ever see what's `#define`d within its own file.

So a second, real mechanism does the cross-file job: every variant
`.c` file defines the same external, non-`static` global,
`dskp_family_active_variant`, initialized to its own `DSKP_VARIANT_ID`
(1/2/3). `llvm-link` — the actual, real whole-program merge step
`eclipse-cc`'s own pipeline already runs, not something added for this
fix — rejects two input modules that each define the same external
global. Confirmed directly against the real toolchain, first with a
throwaway pair of test files (`zz_a.c`/`zz_b.c`, scratch-only, each
defining a same-named global with a different value) and then against
the real `dskp_family_active_variant` symbol itself:

```
$ eclipse-cc -o x.simh dskp_common.c kismet.c vulcan.c main.c
error: Linking globals named 'dskp_family_active_variant': symbol multiply defined!
```

No `.simh` output is produced — the build fails, loudly, before dgasm
ever runs. The same failure was independently reproduced for
Zebra+Vulcan together. This is a real, verified guard, not a
documentation-only convention: pairing any two of the three variant
`.c` files in one `eclipse-cc` invocation fails to link, by
construction, regardless of which two.

## Confirming the original bug is actually fixed

Before writing any of the unified code, the pre-existing conflict
`KISMET_NOTES.md` documented was reproduced directly against the real
toolchain, to confirm what specifically breaks (not just trust the
prose):

```
$ cat zz_a.c
asm("dev DSKP = 027");
int a_fn(void){return 1;}
$ cat zz_b.c
asm("dev DSKP = 027");
int b_fn(void){return 2;}
$ eclipse-cc -o x.simh zz_a.c zz_b.c main.c
eclipse-cc: dgasm failed:
Multiple definitions for symbol DSKP.
```

A genuinely interesting, honestly-reported wrinkle found while doing
this: as the three drivers stood *before* this refactor, `kismet.c`
was the only one of the three that actually emitted a `dev DSKP = 027`
declaration via `asm("dev DSKP = 027")` — `zebra.c` and `vulcan.c` both
deliberately avoided declaring the symbolic device name at all,
pasting the literal text `"027"` into every instruction instead (each
file's own header comment says so explicitly, citing exactly this
uncertainty as the reason). Compiling the pre-refactor `kismet.c` and
`vulcan.c` together, as literally written, was checked directly and
does **not** reproduce the "Multiple definitions" error — it compiles
and assembles cleanly, because only one of the two files ever declared
the symbol in the first place. So the conflict `KISMET_NOTES.md`
reported was real and correctly diagnosed as a structural risk (any
second file that adopted Kismet's own symbolic-declaration idiom would
collide with it, and the two drivers' shared use of the literal device
code `027` was always latent evidence of that), but it was not, at the
moment `KISMET_NOTES.md` was written, already manifesting from the
literal file contents in the repository at that time — worth recording
plainly rather than letting the unification's "fixed!" claim imply a
bug was reproduced from the old files that, read literally, wasn't
quite there yet. What IS unambiguously true, and is what this
unification actually fixes: the underlying shape of the problem (three
files each free to declare `dev DSKP = 027` on their own, with nothing
stopping two of them from doing so) is real, and after unification
there is now exactly one file in the whole tree — `dskp_common.c` —
that contains a `dev DSKP =` line at all, so the "Multiple definitions"
failure mode cannot recur by construction, not merely by continued
good luck or discipline.

After the refactor, the equivalent test (`kismet.c` + `vulcan.c` +
`dskp_common.c`, real files, real toolchain) was re-run and now fails
differently and correctly, at the `llvm-link` sentinel described above
— see the transcript in the previous section. The old dgasm-level
"Multiple definitions for symbol DSKP" message no longer has any way to
occur at all, for any pairing of the three variants, because only one
file (`dskp_common.c`) ever emits that assembler declaration now.

## Verification performed

**Individually, each variant still compiles and assembles cleanly**
through the real `eclipse-cc` → `clang -cc1` → `llvm-link` → `opt` →
`llc` → `reorder_asm.py` → `dgasm -t eclipse_s140 -f simh` pipeline,
producing a loadable `.simh` image with no warnings or errors —
re-confirmed directly in this session for all three, each paired with
`dskp_common.c` and a small scratch test-harness `main()` exercising
both the read and write entry points (not committed, scratch-only,
same convention `VULCAN_NOTES.md`'s own `vulcan_compile_test.c` used):

```
$ eclipse-cc -o zebra_variant.simh  dskp_common.c zebra.c  <scratch main>
$ eclipse-cc -o vulcan_variant.simh dskp_common.c vulcan.c <scratch main>
$ eclipse-cc -o kismet_variant.simh dskp_common.c kismet.c <scratch main>
```

All three produced a `wrote ...simh` line and a real, non-empty output
file (each in the 120-140 KB range).

**A new empirical check this refactor specifically needed and didn't
have before:** whether `dgasm` accepts a *symbolic* `DSKP` device name
combined with a *dynamic*, `"r"`-constrained %-operand in the same
instruction (`dskp_common.c`'s `dskp_doa`/`dskp_doc`/etc. all need
exactly this — none of the three original files had actually tried the
combination: `zebra.c`/`vulcan.c` used "r"-operands but pasted a
literal device code specifically to avoid this question; `kismet.c`
used the symbolic name but only ever against a fixed literal AC operand
via `ELDA`/`ESTA`-loaded globals). Confirmed directly with a standalone
test program before committing to this design, and reconfirmed by
inspecting `kismet.c`'s own real generated assembly (after the
refactor) directly: `llc`'s output contains exactly one `dev DSKP =
027` line and a correct instruction sequence —

```
DOA 0,DSKP
DOC 0,DSKP     (called twice, once per DOC in kismet_seek_and_xfer)
DIA 0,DSKP
DIB 0,DSKP
DOCP 0,DSKP
DOBS 0,DSKP
SKPDN DSKP
```

with the call sequence `kismet_select_and_check` → `dskp_doa`,
`dskp_dib`; then `kismet_seek_and_xfer` → `dskp_docp_seek`, `dskp_doa`,
`dskp_doc` (twice), `dskp_dobs_start`, `dskp_dia` — matching Kismet's
own documented 4-phase protocol (Phase I select+seek, Phase II
position, Phase III select+read/write, Phase IV transfer) exactly, and
confirming the C-level control flow survived the refactor's switch from
global+`ELDA`/`ESTA` to shared-function-call register discipline
intact.

**The symbol-collision fix itself, verified empirically, both
directions:**
- Two variants together (`kismet.c`+`vulcan.c`, and separately
  `zebra.c`+`vulcan.c`, each with `dskp_common.c`) now fail to build,
  correctly and loudly, at the `llvm-link` stage
  (`error: Linking globals named 'dskp_family_active_variant': symbol
  multiply defined!`), before dgasm ever runs — no `.simh` file is
  produced.
- Each single variant, alone with `dskp_common.c`, still builds cleanly
  — see above.

This directly satisfies the task's own verification requirement: two
different variants can coexist in the *same build tree* (all three
`.c` files live in `examples/` together, `git status`/`git log` show
all three tracked side by side) without a source-tree-level collision,
while remaining structurally impossible to link two of them into one
running kernel image by mistake.

## What this unification does NOT claim

Exactly the same caveat every one of the three original drivers already
carried, unchanged: **none of Zebra, Vulcan, or Kismet has ever
executed against real hardware or a simulator.** This project's SIMH
build does not model any DSKP-family controller (re-confirmed, same
grep each original NOTES.md already ran, unchanged result — no
"zebra"/"6060"/"6061"/"6067"/"vulcan"/"6122"/"kismet"/"6160"/"6161"/
"6214"/"dskp" reference anywhere in `~/dev/simh-src/NOVA/`'s `.c`
files). Compile/assemble-clean through the real `eclipse-cc`/`dgasm`
pipeline — now demonstrated for all three variants sitting on the
shared core, plus the new llvm-link-level collision guard — remains the
verification ceiling. Unifying the three drivers' shared substrate into
one file does not, and structurally could not, change that: it is a
source-organization and build-safety fix, not new evidence about
whether the register-level design is correct against real silicon.

For per-generation manual citations, page numbers, and the full "what
was and wasn't checked" detail behind every claim in this file, see
`ZEBRA_NOTES.md`, `VULCAN_NOTES.md`, and `KISMET_NOTES.md` — this file
deliberately doesn't re-derive that citation trail, only the
unification-specific findings on top of it.
