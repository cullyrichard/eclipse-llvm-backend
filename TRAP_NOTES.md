# TRAP (voluntary supervisor-mode entry) — investigation notes

Prompted by a real gap: `MMPU_NOTES.md`'s page-fault work
(`mmpu_fault_probe.s`) and `examples/isr_c_test.c`'s interrupt work both
prove real S/140 mechanisms for **involuntary** entry into supervisor
mode (a hardware fault, a device interrupt) — but a Unix-like OS's
syscall ABI (`open`/`read`/`write`/`fork`/`exec`-equivalents) needs a
**voluntary** one: user code that deliberately, on its own schedule,
asks the kernel to do something. Nothing in this project had that yet.
This note documents the real hardware mechanism found, why it was
chosen over the alternatives, and a hand-verified minimal working
trap with a 2-reason dispatch, in the same single-stepped,
predict-then-confirm style as `MMPU_NOTES.md`.

## The mechanism: `SYC` (System Call), a real, dedicated hardware instruction

The S/140 has an actual, purpose-built instruction for exactly this —
**not** a co-opted illegal-instruction trap or MMPU fault, and not an
I/O-device convention. Found by reading the real manual (Data General
014-000642-02 Rev 02, Apr 1981, via bitsavers.org), not by assumption:

- **Table 2.13** (Ch. 2, p.2-17, "Stack Instructions") lists `SYC` —
  "System Call — Pushes a return block and indirectly places the
  address of the *System Call* handler in the program counter."
- **Table 2.14** (Ch. 2, p.2-18, "Reserved locations in unmapped
  logical address space") — the same table `MMPU_NOTES.md` already
  cited for location 3 (PF HANDLER ADDRESS) — lists **location 2: "SC
  HANDLER ADDRESS (indirectable)."** A fixed, dedicated vector, entirely
  separate from location 1 (I/O HANDLER ADDRESS, `isr_c_test.c`'s
  mechanism) and location 3 (PF HANDLER ADDRESS,
  `mmpu_fault_probe.s`'s mechanism).
- **Table 2.17** (Ch. 2, p.2-20, "Program flow alteration and
  conditional instructions") confirms three assembler mnemonics for the
  same real opcode — `SYC`, `SCL`, `SVC` — "Turns the MAP off if on.
  Pushes a return block onto the stack places address of *System Call*
  handler in program counter." (`SVC` — the same three letters x86-family
  OS programmers know from `SVC`/`syscall` — is not a coincidence of
  naming; DG's own manual uses it as a literal synonym mnemonic for this
  instruction.)
- **Full instruction description, Ch. 4, p.4-72** ("Standard Machine
  Instructions"), `SYC acs,acd`:
  > Pushes a return block and transfers control to the system call
  > handler. If a user map is enabled, the instruction disables it and
  > pushes a return block onto the stack. The program counter in the
  > return block points to the instruction immediately following the
  > *System call* instruction. After pushing the return block, the
  > instruction executes a jump indirect to location 2, which contains
  > the address of the *system call* handler. If this instruction
  > disables a user map, then I/O interrupts cannot occur between the
  > time the *System call* instruction is executed and the time the
  > first instruction of the system call handler is executed.
  >
  > **NOTE:** If both accumulators are specified as AC0, the instruction
  > does not push a return block onto the stack. The contents of AC0
  > remain unchanged.

This is a real, dedicated, general-purpose voluntary-trap instruction —
architecturally the S/140's counterpart to `SVC`/`TRAP`/`syscall` on
other machines, standard hardware present on every real S/140 (not an
optional feature, and not a SIMH-only convenience), confirmed by two
independent primary sources: the manual's own prose (above) and the
real `eclipse_cpu.c`/`dgasm` implementations (below), which agree with
each other and with the manual.

### Why this beats the alternatives the task asked to check

- **Not an illegal-instruction trap.** `SYC` is a real, documented,
  always-available instruction on every S/140 — no portability risk,
  no dependency on which optional instruction sets a given machine
  does or doesn't implement (unlike the Nova-3 `SAVE`/`RTN`/etc.
  emulation-trap fallback `DEBUGGING_NOTES.md` entry 11 already found
  and explicitly did *not* need for this backend's own calling
  convention, because `eclipseemu`'s S/140 model implements those
  natively).
- **Not a software-interrupt I/O device.** No device code, no
  `NIO`/`DIA`/`DOA` dance, no interrupt-system involvement except the
  brief, automatic one-instruction inhibit the instruction itself
  arranges (see below) — simpler and more direct than routing a
  syscall through the I/O interrupt path.
- **Not the MMPU fault mechanism.** This was the task's explicit
  fallback suggestion (deliberately trigger a validity/write-protect
  fault as the trap vector), and it would have worked mechanically —
  but the manual itself (Ch. 2 p.2-30, already quoted in
  `MMPU_NOTES.md`'s page-fault section) says a protection fault's
  **return PC is not reliably correct**: *"A protection fault can occur
  at any point during the execution of an instruction. Therefore, the
  return address in the fifth word of the return block is not always
  correct."* `SYC`, by contrast, is explicit and unconditional in the
  manual about its own return address: *"points to the instruction
  immediately following the System call instruction"* — no caveat, no
  fault-class exception. Verified true below, at two independent call
  sites. A real OS syscall ABI needs a return address it can trust
  unconditionally; the MMPU fault path is documented as not offering
  that, `SYC` is documented as offering exactly that, and the
  difference is real, not just a documentation nuance — confirmed by
  actually exercising both mechanisms' return paths in this project
  now (`mmpu_fault_probe.s` never resumes; `trap_probe.s` does, twice).

There was no genuine tradeoff to document here beyond the above — the
task anticipated "the ISA genuinely has no clean voluntary-trap
instruction and you have to co-opt something," but it does have one,
and it is the cleanest of the three candidates, not a compromise.

### Confirmed against the real emulator, not just the manual's prose

`eclipse_cpu.c` (~line 1798, `if ((IR & 0103777) == 0103510)`):

```c
DisMap = Usermap;
Usermap = 0;
MapStat &= ~1;                                  /* Disable MAP */
i = (IR >> 13) & 3;                             /* ACS field   */
j = (IR >> 11) & 3;                             /* ACD field   */
if (i != 0 || j != 0) {                         /* NOT SYC 0,0 */
    t = (GetMap(040) + 1) & AMASK;
    PutMap(t, AC[0]); t++;
    PutMap(t, AC[1]); t++;
    PutMap(t, AC[2]); t++;
    PutMap(t, AC[3]); t++;
    PutMap(t, (PC & AMASK));
    if (C) PutMap(t, (GetMap(t) | 0100000));
    PutMap(040, t);
    PutMap(041, (GetMap(040) & AMASK));
}
PC = indirect(GetMap(2)) & AMASK;
if (DisMap > 0)
    Inhibit = 3;                    /* 1-instruction interrupt inhibit,
                                        only when the trap came from
                                        real user mode -- matches the
                                        manual's I/O-inhibit sentence
                                        above exactly */
<stack-overflow check, same shape as every prior probe>
```

This matches the manual's prose in every particular: disables the
current user map, pushes AC0-AC3+PC (the identical 5-word return-block
format `MMPU_NOTES.md`'s page-fault section already established and
verified — `mmpu_fault_probe.s`'s handler reads exactly this shape),
jumps indirect through location 2, and — the manual's `AC0,AC0`
exception — the push is skipped iff both AC fields select AC0.

**Cross-checked, not just read**: `dgasm`'s own base encoding for `SYC`
(`opcode.c`: `0b1000011101001000`, `ACS`/`ACD` fields zeroed) computes
by hand to octal `0103510` — exactly `eclipse_cpu.c`'s comparison
constant. Two independent real implementations (the assembler that
produced `trap_probe.simh` and the CPU model that ran it) agree on the
opcode without either having been read against the other first.

**The return instruction, `POPB`** (Ch. 4 p.4-68): *"Returns control
from a System Call routine or an I/O interrupt handler that does not
use the stack change facility of the Vector instruction. Five words
are popped off the stack..."* — a real instruction whose manual
description names `SYC` by name as (one of) its intended partner.
`eclipse_cpu.c` (~line 1525) confirms it pops AC3, AC2, AC1, AC0, then
PC+carry, in that order (reverse of the push), and, critically:
```c
if (MapStat & 1) {
    Usermap = Enable;
    Inhibit = 0;
}
```
— `POPB` is what reactivates user mode on return, *not* automatic:
`SYC`'s dispatch code above unconditionally clears `MapStat`'s bit 0
(`MapStat &= ~1`), so a handler that wants the caller to resume in real
user mode must explicitly re-set that bit (an ordinary `DOA ...,MAP`,
reusing the exact same value that originally enabled the map — `Enable`
itself, which map A/B is active, is untouched by `SYC`) *before*
issuing `POPB`. Verified empirically below, not assumed.

## Calling convention: verified

`SYC`'s `ACS`/`ACD` operand fields carry **no data** — confirmed in
source above, the *only* thing they affect is whether the `AC0,AC0`
special case suppresses the push. So the register convention a syscall
layer uses is entirely a software choice, not something the hardware
dictates. `trap_probe.s` establishes and verifies one, chosen to match
this backend's own existing convention (`DEBUGGING_NOTES.md` entry 11:
AC0 is the return-value register for ordinary compiled calls):

- **Input** (set by the caller before `SYC`): **AC0 = syscall/trap
  reason number, AC1 = one argument.** This works with zero extra
  mechanism — `SYC` doesn't touch the live AC0-AC3 registers at all
  (only copies them to the stack), so the handler reads them exactly as
  the caller left them, the same way `isr_c_test.c`'s handler reads
  `DIB`'s result straight into AC0.
- **Output** (the handler's return value, read by the caller after
  `SYC` resumes): **AC0**, but getting a value there is the one
  genuinely non-obvious part of this convention, and it is the same
  hazard `DEBUGGING_NOTES.md` entry 11 already found and fixed for
  `SAVE`/`RTN`: **`POPB` restores AC0-AC3 by popping the stack's
  snapshot, not from whatever the handler's live registers hold at
  `POPB` time.** A handler that just does `LDA 0,newval` before `POPB`
  accomplishes nothing observable to the caller. The fix, identical in
  shape to entry 11's: load AC2 with the current stack pointer (loc
  `040`'s contents — by hand-arithmetic from the push code above,
  `new_SP = old_SP+5` and the AC0 slot sits at `old_SP+1 = new_SP-4`),
  then `STA 0,-4,2` to overwrite the *stack's* AC0 slot — the same
  AC2-relative addressing idiom this backend's own compiled epilogues
  already use for exactly this purpose.
- **AC2/AC3 pass through automatically.** Unlike `MMPU_NOTES.md`'s
  Phase 2 C API (`mmpu.c`), which had to manually save/restore AC2
  because `LMP` clobbers it with no built-in restore, `SYC`/`POPB`'s
  push/pop *is* a full AC0-AC3 save/restore by construction — a future
  C-callable wrapper gets the compiler's live frame pointer (AC2)
  preserved through the trap for free, with no manual save/restore
  asm needed on either side of the call. (Not exercised by a real C
  test in this increment — see "What's open" below — but a real,
  citable structural advantage over the MMPU-fault/LMP precedent.)

## Empirical verification

`examples/trap_probe.s` (new, committed): genuine user-mode code (the
same `mmpu_usermode_probe.s`-established identity-page-0 entry
mechanism, reused unmodified) executes `SYC` from **two different call
sites**, with **two different reason codes** (0 and 1), each carrying
its own argument. The handler dispatches on the reason (the same
`MOV# 0,0,SZR` zero/nonzero skip idiom `isr_c_test.s` already
established for its own 2-way dispatch), runs genuinely different code
per reason (`path_a`/`path_b`), and returns a distinct, recognizable
value (`0125252` octal = 0xAAAA for reason 0, `0052525` octal = 0x5555
for reason 1) to prove the *right* path ran, not just *some* path.

```
$ dgasm -t eclipse_s140 -f simh -o trap_probe.simh trap_probe.s
$ { cat trap_probe.simh; echo 'set debug trace.log'; echo 'd debug 100003'; \
    echo 'dep PC 50'; echo 'step 60'; echo 'e 221'; echo 'e 222'; \
    echo 'e 223'; echo 'e 224'; echo 'quit'; } | eclipse

HALT instruction, PC: 00077 (STA 0,215)
221:	000111        -- markerA: argA as seen inside path_a
222:	000222        -- markerB: argB as seen inside path_b
223:	125252        -- retA_seen: call site 1's AC0 after returning
224:	052525        -- retB_seen: call site 2's AC0 after returning
```

(`PC: 00077` is SIMH's post-HALT display, not a sign the program didn't
finish — the trace below shows the real final instruction is `HALT` at
address `076`, with the user-mode `A` prefix still present, i.e. the
program ran to a clean, correct stop *while still in real user mode*.)

All four values match hand-predicted expectations exactly. The
instruction trace is the more direct evidence, showing the mechanism
itself, not just before/after memory:

```
 A000066 acs: 001747 000000 000207 000000 0 LDA 0,211
 A000067 acs: 000000 000000 000207 000000 0 LDA 1,212
 A000070 acs: 000000 000111 000207 000000 0 SYC 1,1
  000077 acs: 000000 000111 000207 000000 0 STA 0,215      <- handler, no "A": Usermap forced 0
  ...                                                          (reason 0 -> path_a)
  000111 acs: 125252 000111 000405 000000 0 STA 0,-4,2     <- AC0 stack-slot fixup: 0xAAAA
  000120 acs: 000001 000111 000405 000000 0 DOA 0,MAP      <- re-enable MapStat bit 0
  000121 acs: 000001 000111 000405 000000 0 POPB
 A000071 acs: 125252 000111 000207 000000 0 STA 0,223      <- "A": back in real user mode,
                                                                at PC 071 = the instruction
                                                                *after* SYC (070), not 070
                                                                itself; AC0 = 125252 exactly
 A000072 acs: 125252 000111 000207 000000 0 LDA 0,213
 A000073 acs: 000001 000111 000207 000000 0 LDA 1,214
 A000074 acs: 000001 000222 000207 000000 0 SYC 1,1        <- second, different call site
  000077 acs: 000001 000222 000207 000000 0 STA 0,215      <- same handler, reason 1 this time
  ...                                                          (reason 1 -> path_b)
  000116 acs: 052525 000222 000405 000000 0 STA 0,-4,2     <- 0x5555 this time
  000121 acs: 000001 000222 000405 000000 0 POPB
 A000075 acs: 052525 000222 000207 000000 0 STA 0,224      <- back at PC 075, the instruction
                                                                *after this SYC* (074) -- a
                                                                genuinely different return
                                                                address than the first trap's
 A000076 acs: 052525 000222 000207 000000 0 HALT           <- normal flow continues,
                                                                still in real user mode
```

This directly proves every claim the task asked for:
- **Voluntary, deliberate trap from real user mode** (the leading `A`
  on the `SYC` lines themselves).
- **Correct return to the instruction *after* the trap, not the
  trapping instruction** — `071` after the `070` trap, `075` after the
  `074` trap — at **two genuinely different addresses**, ruling out a
  hardcoded/fixed resumption point.
- **A syscall number reaches the handler and selects genuinely
  different code** (`path_a` vs `path_b`, each storing its own distinct
  marker/return value) — not just "an interrupt happened."
- **User mode is correctly reactivated on return** (`A` prefix resumes
  immediately after each `POPB`) — not left running in supervisor mode,
  which is what would happen without the handler's `DOA` re-enable
  step (see the bug below).

### A real bug found and fixed: the stack-underflow threshold

Every prior stack-using probe in this project (`mmpu_fault_probe.s`,
`mmpu_wpfault_probe.s`) used `spval = 060` and only ever *pushed* once,
then `HALT`ed — never popping the stack back down. `trap_probe.s` is
the first to actually exercise a pop (`POPB`), and the first run (with
the same `spval = 060` every prior probe used) failed exactly there:
`POPB` set `PC = 0` and left every stack-related memory location
completely untouched, rather than resuming at the expected `071`.

Root-caused by single-stepping to the exact instruction before `POPB`
(confirmed loc `40`/`41`/`61`-`65` all held the right pre-pop values —
the push itself was correct) and then stepping exactly one more
instruction (confirmed everything was *still* untouched and `PC` was
now `0`) — narrowing the fault to `POPB`'s own internal logic, not
anything upstream. Reading `eclipse_cpu.c`'s `POPB` handler fully (not
just the part relevant to the happy path) found the real cause: after
popping all 5 words, `POPB` checks
```c
t = GetMap(040);
if (t < 0100000 && t < 0400) {           /* stack underflow */
    pushrtn(PC);
    PC = indirect(GetMap(043));          /* loc 43: stack fault address */
    ...
}
```
— an **underflow** check, distinct from the overflow check every prior
probe's identical-looking code already passed safely. `060` octal (48
decimal) is below `0400` octal (256 decimal), so *every* pop back down
to this probe's stack base spuriously looked like an underflow. Location
`043` (the stack fault handler address) was never initialized by this
probe (or any prior one — none of them needed it), so it held `0`, and
`indirect(0)` resolved straight to physical address `0` — exactly the
observed `PC = 0`. This matches the manual's own documented rule (Ch. 2
pp.2-14/2-15, "Stack Underflow Protection" and "Initializing the Stack
Control Words"): underflow protection triggers whenever the stack
pointer drops below `0400` octal, *unless* bit 0 of both the stack
pointer and stack limit are set to explicitly place the stack in page
zero (not done by any probe in this project) — "Otherwise, start the
stack at a location greater than `401`[octal]."

Fixed by raising `spval` to `0400` octal — comfortably above this
probe's own code and data, and exactly matching the manual's own
initialization rule — with no other change. Confirmed by re-running:
the full trace above is from the *fixed* version. Worth flagging for
any future probe that pops a stack it pushed: `mmpu_fault_probe.s`'s
and `mmpu_wpfault_probe.s`'s `spval = 060` was never actually wrong for
what those two files do (they never pop), but it is **not** a safe
default to copy into anything that does.

## Regression check

Only `examples/trap_probe.s` (new) and this document were added — no
existing file was modified, so nothing else is affected by
construction. Re-run anyway, per this project's standing practice:
`examples/mmpu_fault_probe.s` re-run directly against the real
`eclipse` binary: byte-identical to its own documented transcript
(`61:001747 62:000000 63:000110 64:000000 65:000070 112:000000`). Zero
`dgasm-src`/`eclipse-cc`/`llvm-project`/`simh-src` changes anywhere in
this investigation — everything above is exercising existing, unmodified
real hardware behavior.

## What's open

- **A C-callable syscall wrapper.** `trap_probe.s` is hand-assembled,
  the same starting point `mmpu_probe.s` was for the MMPU before
  `mmpu.c`'s Phase 2 C API. Structurally this should be *easier* than
  that was (`SYC`/`POPB` save/restore all of AC0-AC3, including the
  live frame pointer AC2, automatically — no manual `AC2`/`AC3`
  save/restore asm needed, unlike `mmpu.c`'s `LMP`-clobbers-AC2 fix).
  The real open question is how the handler itself gets built and
  installed: it cannot be an ordinary `__attribute__((interrupt))`
  function (that lowering targets the I/O-interrupt `NIOS 077`+`JMP @0`
  convention, not `SYC`'s `POPB`-based one), so it would need either a
  hand-written asm handler installed at location 2 the same
  hand-edited-`.s` way `isr_c_test.c`'s vector at location 1 is
  installed, or new backend support for a `POPB`-shaped return
  convention. Not attempted here — scoped, not started, matching
  `MMPU_NOTES.md`'s own practice of naming a real next step rather than
  rushing a fragile version of it into this increment.
- **A trap taken while already in supervisor mode.** `SYC`'s dispatch
  code only sets the 1-instruction `Inhibit` when `DisMap > 0` (i.e.
  the trap came from real user mode) — a `SYC` executed from inside
  supervisor code (`DisMap == 0`) takes a different, unexercised
  path through the same instruction. Not tested here.
- **Nested traps** (a `SYC` executed from inside this probe's own
  handler, before the first one returns). The stack mechanism looks
  like it should support this directly (each `SYC` just pushes another
  5-word block; `POPB` pops in LIFO order automatically), but that is
  an untested expectation, not a verified one.
- **`MapIntMode` / interrupt interaction during a trap.** `SYC`'s own
  `Inhibit = 3` briefly blocks interrupts right after the trap, but
  what happens if a *different* interrupt is already pending, or fires
  while a syscall handler is running with the user map disabled, is
  unexplored — the same open item `MMPU_NOTES.md`'s own `MapIntMode`
  section already flagged as needing different (non-single-step-able)
  tooling, for the same wall-clock-timing reason. **Partially resolved
  — see "SYC and MapIntMode: can a syscall handler recover the
  pre-trap user map?" below**: whether `SYC` alone gives a handler
  enough state to resume the correct map is now verified (yes, with a
  specific discipline). Whether that still holds with a real armed
  scheduler timer interrupting *during* a syscall handler remains
  exactly as open as this bullet originally said.
- **A real syscall table.** This probe hardwires exactly two reasons
  with `if/else`-shaped dispatch, sufficient to prove the mechanism
  generalizes, but nowhere near a real syscall-number-indexed jump
  table a kernel would actually want (the manual's own `DSPA`
  instruction — "Dispatch... uses the integer as an index into a table
  and places the resulting address in the program counter", Table 2.17
  — looks like the right real-hardware building block for that, not
  investigated further here).

## SYC and MapIntMode: can a syscall handler recover the pre-trap user map?

**The gap this closes**, found but not fixed during `PROCESS_NOTES.md`'s
scheduler work (that document's own closing section 8): `MMPU_NOTES.md`'s
`MapIntMode` investigation proved a real device-interrupt handler can
recover *which* user map (A or B) was active pre-interrupt, by reading
`DIA 0,MAP` as its very first action before touching the `MAP` device
again (`mmpu_intmode_resume_probe.s`). `PROCESS_NOTES.md` flagged, but
did not verify, that `SYC`'s own dispatch does **not** do the
`MapIntMode = MapStat` save step the generic interrupt path does — so a
process that executes `SYC` mid-timeslice under `scheduler_probe.s`'s
scheduler had no *verified* way to recover its own pre-trap map identity.
This section is that verification.

**Bottom line, stated up front**: **yes, `SYC` is safe to use from a
process running under the preemptive scheduler, for map-identity
recovery specifically** — with one precise, non-negotiable discipline,
empirically verified below, not assumed: the handler's first action
must be `DIA 0,MAP` (exactly as the interrupt path already requires),
*and*, unlike the interrupt path, the handler must explicitly force
bit 0 (User Enable) on before reprogramming `MapStat` — it must never
trust `DIA`'s bit 0 for this vector. Do both, and the exact pre-trap map
(A or B) is recovered correctly every time, confirmed by a real,
single-stepped probe with three call sites, one deliberately wrong.
Skip the bit-0 fix and the failure mode is not "resumes under the wrong
map" (the interrupt case's failure mode) — it is worse and different:
the process silently keeps running as unmapped, unprotected supervisor
code, with no MMPU enforcement at all. What remains **unverified**: the
combination of `SYC` with a *simultaneously armed, live scheduler timer*
(a real `SYC` interrupted by, or racing, a real PIT interrupt). See
"What's still unverified" at the end of this section.

### Step 1: re-reading the manual specifically for this question

The task that prompted this section was explicit: don't assume the
manual is silent on `SYC`'s Map Status behavior just because
`TRAP_NOTES.md`'s original pass didn't focus on it — go back and check.
Done here by fetching the real PDF
(`bitsavers.org/pdf/dg/eclipse/014-000642-02_S140_PgmrRef_Apr81.pdf`)
and reading the actual page images (not OCR) at the three places this
question could plausibly be answered:

- **Ch. 2, p.2-17** (Table 2.13, "Stack Instructions", PDF page 21):
  the `SYC` row is a one-line summary — "Pushes a return block and
  indirectly places the address of the *System Call* handler in the
  program counter." No mention of Map Status.
- **Ch. 4, p.4-72** (PDF page 78) — the full `SYC` instruction
  description, already quoted in this document's own opening section,
  re-read here specifically hunting for any Map-Status-preservation
  language: *"Pushes a return block and transfers control to the
  system call handler. If a user map is enabled, the instruction
  disables it and pushes a return block onto the stack. ... If this
  instruction disables a user map, then I/O interrupts cannot occur
  between the time the System call instruction is executed and the
  time the first instruction of the system call handler is executed."*
  Read in full, including the surrounding page and the `AC0,AC0` NOTE:
  **the manual genuinely says nothing about preserving which map was
  disabled, or about any status/latch a handler could read back to
  recover it.** This is a real, now-confirmed negative finding, not an
  absence-of-evidence assumption — the page was read directly, not
  inferred from `TRAP_NOTES.md`'s earlier, narrower pass over it.
- **Ch. 5, pp.5-85/5-86** ("Read Map Status", `DIA ac,MAP`, PDF pages
  92-93) and **p.5-87** ("Load Map Status", `DOA ac,MAP`, PDF page 95):
  read in full for the first time in this project (prior work cited
  `DIA`/`DOA` only via `eclipse_cpu.c`, never the manual's own bit
  table for them) — and this is where the real, citable answer to the
  underlying question turns out to live:
  - **Bit 15, "User Mode"** (DG numbering; this is `eclipse_cpu.c`'s
    bit 0, LSB-first — the same bit `MMPU_NOTES.md`'s `MapIntMode`
    section already identified): *"If 1, **the last I/O interrupt**
    occurred while in user mode."* The manual's own words scope this
    bit explicitly to **I/O interrupts** — not traps, not `SYC`, not
    "the last time the map was disabled" in general. This is a real,
    citable, primary-source reason (not just an `eclipse_cpu.c`
    reading) that a handler cannot trust this bit after a `SYC`: the
    hardware manual itself never promises it tracks anything but the
    interrupt path, and `eclipse_cpu.c`'s implementation (below)
    matches that scoping exactly.
  - **Bit 13, "A/B"**: *"If 0, the last **Load Map Status** instruction
    enabled user map A. If 1, the last Load Map Status instruction
    enabled user map B."* This is the field this section's whole fix
    depends on, and the manual's own wording is exactly why it works
    for `SYC`: it is defined as tracking the last `DOA ...,MAP`
    ("Load Map Status") instruction, full stop — not "the map active
    when an interrupt landed," not anything interrupt-specific at all.
    It is architecturally a plain, persistent field of the live
    `MapStat` register, unrelated to `MapIntMode`. Whatever last wrote
    it (a `DOA`) is what it reads back as, until the next `DOA` —
    which is precisely why it survives `SYC` (which never issues a
    `DOA`) exactly as it survives a device interrupt (which also never
    issues one before the handler's own `DIA`).
  - This retroactively sharpens `MMPU_NOTES.md`'s own `mmpu_intmode_
    resume_probe.s` finding, which called the bit-13 survival "purely
    an emulator-source-and-trace-verified finding... the manual does
    not describe this side channel at all." That characterization was
    about the *interrupt* case specifically and remains accurate for
    it (the manual's interrupt-handling prose never mentions bit 13
    surviving); but bit 13's *own* definition, read directly here for
    the first time, independently corroborates *why* the mechanism
    works, for either vector: it was never an interrupt-specific latch
    to begin with, just a plain status field that happens not to get
    touched unless something explicitly reloads it. (Not editing
    `MMPU_NOTES.md` itself to reflect this per this task's scope — noted
    here, where the `SYC`-specific consequence actually matters.)

### Step 2: cross-checking `eclipse_cpu.c` precisely for what `SYC` does and doesn't touch

Confirmed directly against `~/dev/simh-src/NOVA/eclipse_cpu.c`, not
inferred from the manual alone:

**`SYC`'s dispatch** (`~line 1798-1829`):
```c
if ((IR & 0103777) == 0103510) {           /* SYC: System call */
    DisMap = Usermap;
    Usermap = 0;
    MapStat &= ~1;                          /* Disable MAP */
    ...push AC0-AC3+PC as usual...
    PC = indirect(GetMap(2)) & AMASK;
    if (DisMap > 0)
        Inhibit = 3;                        /* 1-instruction inhibit */
    ...
}
```
**No `MapIntMode = MapStat` anywhere in this block** — confirmed by
reading the full dispatch, not just the excerpt `TRAP_NOTES.md`'s
opening section already had. Compare the generic interrupt path
(`~line 783-787`, already quoted in `MMPU_NOTES.md`):
```c
MapIntMode = MapStat;                       /* Save Status as it was */
Usermap = 0;
MapStat &= ~1;
```
Both clear **only** bit 0 of `MapStat` (`&= ~1`) — identical in shape.
The interrupt path additionally snapshots the *entire* pre-clear value
into `MapIntMode` first; `SYC` does not. This is the exact, now
source-confirmed shape of the gap `PROCESS_NOTES.md` flagged but never
verified.

**`DIA` (Read Map Status, `~line 5176-5187`)**:
```c
case ioDIA:
    AC[dstAC] = MapStat & 0xFFFE;           /* bits 1-15, bit 0 forced 0 */
    if (MapIntMode & 1)                     /* Bit 15 is mode asof last int */
        AC[dstAC] |= 1;
    break;
```
Bits 1-15 of the result — including bit 13, the A/B select — come
straight from the **live** `MapStat`, never routed through
`MapIntMode` at all. Only bit 0 is. This is the mechanical reason the
Step 1 manual citations above translate directly into working code:
bit 13 was never part of any interrupt-specific save/restore contract
in the first place, for either the manual or the source.

**`DOA` (Load Map Status, `~line 5188-5203`)**: `MapStat = AC[dstAC]`
(a full overwrite) plus `MapIntMode = 0` — confirmed, matching
`MMPU_NOTES.md`'s own citation exactly. This is why the "first `DIA`,
before any other `MAP`-device access" discipline is just as
load-bearing for `SYC` as it already was for interrupts: the instant
any `DOA` runs, bits 1-15 (bit 13 included) are gone, replaced by
whatever that `DOA` wrote.

**The one genuine asymmetry between the two vectors**: since `SYC`
never sets `MapIntMode`, `DIA`'s bit-0 OR-in after a `SYC` reflects
whatever `MapIntMode` last held from some *unrelated* prior real
interrupt (or its power-on-reset value, 0, if none has ever fired) —
not this trap's own pre-trap bit 0. This is not actually a practical
problem, though, because bit 0 (User Enable) is not genuinely unknown
information for a `SYC` handler the way A/B identity is: `SYC`'s own
dispatch only sets `Inhibit = 3` (i.e., "a real trap from user mode
occurred") `if (DisMap > 0)` — so for exactly the case this section
cares about (a process running under the scheduler's timeslice, in
real user mode, voluntarily trapping), bit 0 is **architecturally
guaranteed to have been 1** immediately pre-trap; there is nothing to
"recover," only a known constant to reassert. Verified, not just
argued, below.

### The probe: `examples/syc_intmode_probe.s`

Three call sites through **one generic handler** (no per-call-site
special-casing — the handler branches purely on a `reason` value in
AC0, the same convention `trap_probe.s` established), reusing
`mmpu_context_switch_probe.s`'s exact, already-verified map data (map A
→ logical page 2 → phys page 0150 octal; map B → phys page 0044 octal)
and `trap_probe.s`'s exact `SYC`/`POPB`/identity-page-0 mechanism:

1. **Enter user mode under map A, `SYC` with reason 0 ("informed")** —
   handler does `DIA 0,MAP` first (capturing `mapstat_raw`), then
   reconstructs the full resume value by clearing bit 0 (`AND` with
   `0177776`, so a stale `MapIntMode`-sourced bit 0 can't corrupt
   anything via carry) and adding 1 back (`ADI 0,1`) — forcing bit 0
   on as the known constant, not trusting `DIA` for it — then `DOA`s
   that reconstructed value and `POPB`s.
2. **Return to supervisor (`NIOP`), enter user mode under map B, `SYC`
   with reason 0 again** — the *same* handler code, now recovering B
   instead of A, with nothing in the handler itself hinting which map
   "should" be active.
3. **Return to supervisor, enter user mode under map A a third time,
   `SYC` with reason 1 ("naive")** — deliberately skips the bit-0 fix,
   using `mapstat_raw` exactly as `DIA` returned it (bit 0 = 0, since
   no real interrupt had ever fired in this run to set `MapIntMode`).

Assembled and run against the real `eclipse` SIMH binary, single-step
traced (`d debug 100003`):
```
$ dgasm -t eclipse_s140 -f simh -o syc_intmode_probe.simh syc_intmode_probe.s
$ { cat syc_intmode_probe.simh; echo 'set debug trace.log'; echo 'd debug 100003'; \
    echo 'dep PC 50'; echo 'step 90'; \
    echo 'e PC'; echo 'e 320200'; echo 'e 110200'; echo 'e 4200'; echo 'quit'; } | eclipse

HALT instruction, PC: 00132 (STA 0,233)
PC:	00132
320200:	001234
110200:	013056
4200:	006060
```
- `320200` (phys page 0150 octal, map A's target) = `001234` octal —
  exactly `markerA`. Call site 1 (informed, map A) correctly recovered
  and resumed under A.
- `110200` (phys page 0044 octal, map B's target) = `013056` octal =
  5678 decimal — exactly `markerB`. Call site 2, using the *identical*
  handler code that had just handled map A, correctly recovered and
  resumed under B instead — direct proof this is genuine hardware
  readback, not a call-site-specific hardcoded assumption.
- `4200` (plain logical/physical page 2, offset 0200 — i.e.
  `Usermap==0` addressing) = `006060` octal — exactly `markerNaive`.
  Call site 3's write never reached phys page 0150 (map A's target,
  which still correctly holds only call site 1's `markerA`, confirmed
  unmodified by this same dump) — it landed at the plain, untranslated
  address instead. See below for why.

**The instruction trace independently confirms the mechanism, not just
the memory dump** (excerpted from `examples/trace.log`, this run):

Call site 1 — trapped from real user mode (`A` prefix), `DIA` reads
back raw `MapStat = 0` (map A, bits 1-15 all clear since `enterA`'s
only set bit was bit 0), reconstructed to `1` (`AND` → 0, `ADI` → 1),
`DOA 0=1` exactly reproduces the original `enterA` value, and `POPB`
resumes with the `A` prefix restored:
```
 A000101 acs: 000000 001751 000217 000000 0 SYC 1,1
  000134 acs: 000000 001751 000217 000000 0 DIA 0,MAP
134 DIA 0=0 (Read Map Status)
  000144 acs: 000000 177776 000217 000000 0 AND 1,0
  000145 acs: 000000 177776 000217 000000 0 ADI 1,0
  000153 acs: 000001 177776 000217 000000 0 DOA 0,MAP
153 DOA 0=1 (Load Map Status)
 A000102 acs: 000000 001751 000217 000000 0 LDA 1,230
 A000103 acs: 000000 001234 000217 000000 0 ESTA 1,4200
```

Call site 2 — trapped from map B (`B` prefix), `DIA` reads back raw
`MapStat = 4` (bit 13/A-B-select is the only bit set, since `enterB`
was `1+4=5` and bit 0 is always masked out of `DIA`'s bits-1-15 field
regardless), reconstructed to `5`, `DOA 0=5` exactly reproduces the
original `enterB` value, and `POPB` resumes with the **`B`** prefix —
the same handler code, correctly distinguishing the two maps with zero
advance knowledge baked in:
```
 B000113 acs: 000000 003722 000217 000000 0 SYC 1,1
  000134 acs: 000000 003722 000217 000000 0 DIA 0,MAP
134 DIA 0=4 (Read Map Status)
  000153 acs: 000005 177776 000217 000000 0 DOA 0,MAP
153 DOA 0=5 (Load Map Status)
 B000114 acs: 000000 003722 000217 000000 0 LDA 1,231
 B000115 acs: 000000 013056 000217 000000 0 ESTA 1,4200
```

Call site 3 — trapped from map A again (`A` prefix on the `SYC`
itself, confirming the trap really did come from real user mode), same
raw `DIA 0=0` as call site 1 (correctly, since it's map A again), but
the naive path skips reconstruction and `DOA`s the raw value directly:
```
 A000125 acs: 000001 005673 000217 000000 0 SYC 1,1
  000134 acs: 000001 005673 000217 000000 0 DIA 0,MAP
134 DIA 0=0 (Read Map Status)
  000153 acs: 000000 005673 000217 000000 0 DOA 0,MAP
153 DOA 0=0 (Load Map Status)
  000154 acs: 000000 005673 000217 000000 0 POPB
  000126 acs: 000001 005673 000217 000000 0 LDA 1,232
  000127 acs: 000001 006060 000217 000000 0 ESTA 1,4200
  000131 acs: 000001 006060 000217 000000 0 HALT
```
**The resume line (`000126`) and every line after it carry no `A`/`B`
prefix at all** — direct, trace-level proof that `POPB`'s own
`if (MapStat & 1) { Usermap = Enable; ... }` (already cited in this
document's opening section) never fired, because `DOA 0=0` left bit 0
off. The process did not resume under the wrong map (this section's
opening claim) — it resumed as **plain supervisor code**, unmapped,
with the MMPU fully disabled, and the following `ESTA 1,4200` executed
as an ordinary physical write instead of a translated one, landing at
address `04200` directly rather than being routed through `Map[1][2]`
to phys page 0150 — exactly what the final memory dump above confirms.
This is a genuinely different, and arguably more dangerous, failure
mode than the interrupt case's "wrong map, no fault, no warning" — it
is "no map enforcement at all, no fault, no warning."

### Answer to the scoping question

**Is `SYC` safe to use from a process running under
`scheduler_probe.s`'s preemptive scheduler?** For map-identity recovery
specifically — **yes**, now verified with the same rigor as the
interrupt case, via a real, three-call-site, single-stepped probe with
one deliberately-broken control case:

1. The handler's first action must be `DIA 0,MAP`, exactly as
   `MMPU_NOTES.md`'s interrupt-path contract already requires — bits
   1-15 (A/B identity included) survive `SYC` exactly as they survive a
   device interrupt, for the same underlying reason (neither vector
   issues a `DOA`), now confirmed by both source (`eclipse_cpu.c`) and
   the manual's own bit-13 definition (Step 1 above).
2. Unlike the interrupt path, the handler must **not** trust `DIA`'s
   bit 0 — it must explicitly reconstruct/force it to 1 before
   `DOA`-ing the result back. This is safe and correct because bit 0's
   true pre-trap value is not actually unknown: `SYC` only sets
   `Inhibit = 3` (i.e., only reaches this code path at all in a way
   that matters here) when trapping from genuine user mode, which
   requires bit 0 to already have been 1 to get there.
3. Skipping step 2 is not a smaller version of the interrupt case's
   failure — it silently drops all MMPU enforcement for the "returned"
   process instead of merely picking the wrong address space, verified
   directly via the trace's missing `A`/`B` prefixes and the marker's
   landing at a plain physical address instead of a translated one.

A real syscall handler for this project's future syscall ABI should
therefore open with exactly: `STA`-off the live ACs it needs to
preserve, `DIA 0,MAP` before anything else touches `MAP`, save that
value, and — when it's time to return — reconstruct
`(saved_value & 0177776) + 1` (or equivalently, unconditionally set bit
0) before `DOA`-ing it back and `POPB`-ing. `PROCESS_NOTES.md`'s TCB
(`examples/proc.h`) already has a `mapstat` field sized and shaped
exactly for this value, unchanged.

### What's still unverified

- **The actual scheduler-interaction case**: a process executing `SYC`
  while `scheduler_probe.s`'s PIT timer is genuinely armed — i.e.,
  whether a real device interrupt landing *during* the brief window
  between a `SYC` trap and its handler's own `DIA` (or during the
  handler's execution generally) can corrupt this recovery mechanism.
  This probe deliberately isolates `SYC` from any armed timer, per this
  document's own task scope ("at minimum prove `SYC` correctly
  identifies and returns to the right map in isolation, and clearly
  note the scheduler-interaction case remains unverified" — doing so
  here, honestly, rather than assuming the isolated result generalizes).
  Two structural facts are worth naming as reasons to expect it's
  survivable, without treating them as a substitute for an actual test:
  `SYC`'s own `Inhibit = 3` blocks the interrupt check for (empirically,
  per this run's trace addressing) more than one instruction right after
  the trap, covering at least the handler's first instruction; and this
  project's scheduler-handler design always writes captured state to a
  process's own TCB memory immediately, before any window where a
  second interrupt could plausibly interleave and disturb *live
  register* state. Neither of these has been tested under a real,
  non-deterministically-timed PIT interrupt actually racing a `SYC` —
  doing so would need the same kind of deliberately-staged, SCP-deposit
  interrupt injection `mmpu_intmode_resume_probe.s` and
  `scheduler_probe.s` already established as tooling, combined for the
  first time with a `SYC` trap in flight, not attempted in this
  increment.
- **Nested `SYC`** (a second `SYC` from inside this handler) and **a
  `SYC` taken from inside supervisor code** (`DisMap == 0`) remain
  exactly as unverified as `TRAP_NOTES.md`'s original "What's open"
  section already said — this section adds no new information about
  either.
- **Real S/140 hardware match**: like `mmpu_intmode_resume_probe.s`'s
  own equivalent caveat, this section's `eclipse_cpu.c` source-reading
  is trusted as far as this project's whole verification methodology
  already trusts SIMH; the manual's own bit-13/bit-15 definitions (Step
  1) independently corroborate the *shape* of the finding, but do not
  by themselves confirm SIMH's specific `SYC` dispatch code against a
  real S/140 — no real-hardware access exists for this project to check
  further.

### Regression check

`dgasm-src` CTest, re-run after this addition: **315 tests, 314
passing, 1 Not Run** (`memcheck_hello`, `valgrind` still not installed
— unchanged from every prior baseline). `examples/sizeof_check.c`
through the full `eclipse-cc` pipeline: byte-identical (`1 2 2 4 2 4 10
14`). `examples/trap_probe.s` and `examples/mmpu_intmode_resume_probe.s`
both re-run directly against the real `eclipse` binary: byte-identical
to their own documented transcripts. Zero `dgasm-src`/`eclipse-cc`/
`llvm-project`/`simh-src` changes — `examples/syc_intmode_probe.s` (new)
and this section are the only additions.
