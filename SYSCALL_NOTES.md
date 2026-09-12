# Syscall ABI + user/kernel runtime split, and the SYC/scheduler race

Closes out the four-phase dependency chain `MapIntMode` (`MMPU_NOTES.md`)
-> trap/`SYC` (`TRAP_NOTES.md`) -> preemptive scheduler
(`PROCESS_NOTES.md`) -> a real syscall ABI (this document). Builds a
minimal, fixed syscall table on top of `TRAP_NOTES.md`'s verified `SYC`
mechanism, wraps `examples/console.c`/`examples/blockdev.c`/
`examples/dsk.c` (not stubs), adds the user-mode-side C library the
project didn't have, and — the part that matters most, per this
document's own task — investigates the one thing `TRAP_NOTES.md`
explicitly left open: *does a syscall trap taken while
`scheduler_probe.s`'s PIT timer is armed interact safely with the
scheduler's own preemption logic?*

**Bottom line, stated up front**: **no.** The syscall ABI itself works
correctly end to end (real console I/O, real block I/O, a real
compiled user-mode C wrapper library, all exercised through the actual
toolchain and a real SIMH run). But composing it with
`scheduler_probe.s`'s existing, already-verified scheduler exposes a
**real, demonstrated, unresolved race**: a PIT interrupt landing at
almost any point during a syscall's execution (not just a narrow
window) causes the scheduler to silently misidentify *which* process
trapped, and — confirmed by a direct, single-stepped SIMH run, not
inferred — this corrupts one process's saved context with the other's,
while the second process's own progress is silently lost. This is not
a corner case that was merely "not reached" in the time available; it
was reached, deliberately, via deterministic interrupt injection, and
the corruption is visible directly in the TCB memory dump. See
"The verified race" below for the full evidence and "What remains open"
for an honest accounting of what wasn't additionally exercised.

## 1. The syscall ABI

### Calling convention

`AC0` = syscall number (`examples/syscall.h`'s `SYS_*`), `AC1` = arg1,
`AC2` = arg2 — extending `TRAP_NOTES.md`'s own `trap_probe.s` convention
(`AC0` = reason, `AC1` = one argument) by one more register, safe to do
because `SYC` pushes and `POPB` restores `AC0`-`AC3` as a single
hardware unit regardless of how many a given handler uses
(`TRAP_NOTES.md`: *"AC2/AC3 pass through automatically"*) — this ABI is
the first thing in this project to actually spend that already-verified
headroom rather than just note it exists. `AC0` on return is the
syscall's result, via the same "patch the stack's saved `AC0` slot
before `POPB`" fixup `trap_probe.s`/`syc_intmode_probe.s` already
established and verified.

```
SYS_PUTCHAR   0   arg1 = character.           returns 0.
SYS_GETCHAR   1   no args.                    returns the character read.
SYS_BLKREAD   2   arg1 = block number.        returns dsk status (0 = ok).
SYS_BLKWRITE  3   arg1 = block number.        returns dsk status (0 = ok).
```

### Kernel side: `examples/syscall.h`/`syscall.c`, `examples/syscall_entry.s`

`syscall_entry.s` is hand-written Eclipse S/140 assembly, **not**
compiled from C — same reasoning `TRAP_NOTES.md`'s own "What's open"
section already gave: a normal compiled function's `SAVE`-based
prologue/`RTN` epilogue is the wrong shape for a hardware jump target
that must return via `POPB`, reading its own arguments straight out of
`AC0`-`AC2` rather than a pushed frame. It is installed at memory
location 2 (SC HANDLER ADDRESS) by `syscall_install()`
(`examples/syscall.c`), which computes the handler's address at runtime
via `ELEF` (matching `examples/mmpu.c`'s own `LMP`-wrapper idiom) rather
than needing a `var NAME = someLabel` (address-of-a-label initializer)
whose validity as dgasm syntax was never independently confirmed —
sidestepped entirely, not assumed to work.

`__syscall_handler`'s discipline, reusing `TRAP_NOTES.md`'s "SYC and
MapIntMode" section's already-verified contract unchanged:

1. Capture `AC0`(reason)/`AC1`(arg1)/`AC2`(arg2) immediately.
2. `DIA 0,MAP` — the first MAP-device access since `SYC` fired.
3. Hand off to compiled C (`syscall_dispatch(reason)`, with
   `sysarg1`/`sysarg2` globals carrying the rest) via the ordinary
   `SAVE`/`RTN` calling convention's own `EJSR`-based call/push/pop —
   the exact mechanics `eclipse-toolchain/rt/eclipse_hwfloat.s`'s own
   `EJSR` calls into `ieee754_to_hexfloat32`/`__hwf_fits_pos16` already
   established and verified, reduced here to this ABI's own
   **one**-argument case. (`sysarg1`/`sysarg2` are deliberately plain
   globals, not extra call arguments, so this hand-written-asm-into-
   compiled-C call stays inside the only shape this project has
   actually verified — 1 or 2 arguments — rather than assuming an
   untested 3-argument shape works by extrapolation.)
4. Patch the `SYC`-pushed stack's `AC0` slot with `syscall_dispatch`'s
   return value, reloading `AC2` = the current stack pointer **freshly,
   after** the call returns (the call's own callee clobbers `AC2` as
   its own frame pointer — the same "`AC2` is the live frame pointer,
   and an ordinary call does not preserve the caller's value in it"
   hazard `examples/mmpu.c`'s header comment already found and
   saved/restored for `LMP`; here the call itself is the clobbering
   event, so reloading afterward is the whole fix).
5. Reconstruct `MapStat`: bits 1-15 from the saved `DIA` readback, bit 0
   forced on explicitly (never trusted from `DIA` — see `TRAP_NOTES.md`
   for why this is correct for a trap from real user mode). `DOA` it
   back.
6. `POPB`.

`syscall.c`'s `syscall_dispatch()` wraps already-verified kernel code,
not stubs: `SYS_PUTCHAR`/`SYS_GETCHAR` call `console_putchar`/
`console_getchar` (`examples/console.c`, `BLOCKDEV_NOTES.md`) directly;
`SYS_BLKREAD`/`SYS_BLKWRITE` call `blockdev_read`/`blockdev_write`
(`examples/blockdev.c`) against a real `dsk_dev` (`examples/dsk.c`),
through a fixed kernel-side buffer `sysbuf` — see "What this syscall ABI
does not attempt" below for why a general user-buffer copy wasn't
built. Plain `if`/`else if`, not `switch`, in `syscall_dispatch` — no
example anywhere in this codebase has confirmed `switch` lowers
correctly on this backend, and every prior dispatch in this project
(`trap_probe.s`'s `path_a`/`path_b`, `syc_intmode_probe.s`'s
`h_informed`/`h_naive`, `scheduler_probe.s`'s `was_A`/`was_B`) uses an
explicit compare-and-branch chain instead; matched here at the C level
rather than introducing an unverified construct into the one function
this whole ABI funnels through.

### User side: `examples/ulib.h`/`ulib.c` — the runtime split this project didn't have

`eclipse-toolchain/rt/` (unmodified) stays the supervisor-mode/direct-
hardware-access runtime it always was. `ulib.c` is the new, separate
user-mode runtime: every function in it is a thin `SYC`-issuing wrapper
(`sys_putchar`/`sys_getchar`/`sys_blkread`/`sys_blkwrite`), zero direct
device I/O. `__syscall()`, the one function that actually issues `SYC`,
follows `examples/mmpu.c`'s own established, verified pattern rather
than `"r"`-constrained inline-asm operands: every value crosses the
asm boundary through named globals (`ELDA`/`ESTA` by literal C name),
never through an operand — `mmpu.c`'s own header comment already found
that mixing a hardcoded-by-convention register (`SYC`'s
`AC0`=reason/`AC1`=arg1/`AC2`=arg2, exactly the same "architectural
convention, not operand encoding" shape `LMP`'s own `AC0`/`AC1`/`AC2`
already has) with `"r"`-constrained operands the register allocator
could independently place in `AC0`-`AC2` risks an unverifiable
collision (*"no clobber-list or fixed-register constraint support has
been confirmed to exist"* on this backend). `AC2`/`AC3` are explicitly
saved/restored around the `SYC` block for the same reason
`mmpu_read_far`/`mmpu_write_far` already do it for `LMP`: `__syscall()`
itself writes `AC2` (to stage `arg2`), clobbering its own live compiled
frame pointer, which its own epilogue needs again afterward.

## 2. Verification: the ABI works end to end (`examples/syscall_test.c`)

Compiled and run through the **real** toolchain, not a hand-assembled
stand-in — `syscall.c`, `console.c`, `blockdev.c`, `dsk.c`, `ulib.c`,
`syscall_test.c` compiled via `clang -cc1` (one per source, mirroring
`eclipse-cc`'s own pipeline exactly) + `llvm-link` + `opt
-internalize,globaldce` (protecting `main`,`syscall_dispatch`, plus
whatever else the same iterative undefined-symbol retry loop
`eclipse-cc` itself uses finds necessary — this run needed `sysarg2`
added) + `llc`, with `examples/syscall_entry.s`'s raw text concatenated
onto `llc`'s output **before** `reorder_asm.py` runs — the exact same
place/order `eclipse-cc` already concatenates
`eclipse-toolchain/rt/eclipse_hwfloat.s` onto hardware-float builds, for
the identical reason (mixing hand-written asm the compiler can't emit
with compiled C). `eclipse-cc` itself has no flag for accepting a
hand-written `.s` source (every source it's given goes through
`clang -cc1`, which cannot compile `syscall_entry.s`'s `POPB`/`SYC`-
return-block-aware code) — this recipe is the manual equivalent,
documented here so it's reproducible:

```
$ clang -cc1 -triple eclipse-dg-none -nostdsysteminc \
    -isystem "$(clang -print-resource-dir)/include" \
    -I eclipse-toolchain/rt/include -I eclipse-toolchain/rt \
    -emit-llvm <each of syscall.c console.c blockdev.c dsk.c ulib.c \
                syscall_test.c eclipse-toolchain/rt/eclipse_rt.c> -o <file>.ll
$ llvm-link -S <all the .ll files> -o merged.ll
$ opt -S -passes="internalize,globaldce" \
    -internalize-public-api-list=main,syscall_dispatch,sysarg2 \
    merged.ll -o stripped.ll
$ llc -mtriple=eclipse-dg-none -filetype=asm stripped.ll -o out.s
$ cat examples/syscall_entry.s >> out.s
$ python3 eclipse-toolchain/reorder_asm.py out.s out_r.s
$ dgasm -t eclipse_s140 -f simh -o syscall_test.simh out_r.s
```

Run for real:
```
$ { echo 'attach dsk syscall_test.img'; cat syscall_test.simh; \
    echo 'dep PC 50'; echo 'send "Z"'; echo 'run 50'; echo 'quit'; } \
  | ~/dev/simh-src/BIN/eclipse

HI
Z
wstatus=0 rstatus=0 rbuf0=42424 rbuf1=12121

HALT instruction, PC: 00057 (DSZ -121,2)
```

`HI` — two `sys_putchar()` calls, real console output, reaching
`console_putchar` through `SYC` -> `__syscall_handler` ->
`syscall_dispatch` -> `console_putchar`, not a direct TTO write anywhere
in `syscall_test.c`/`ulib.c`. `Z` — `sys_getchar()` correctly returning
the character SIMH's own `SEND` command injected into TTI, then echoed
back via a second `sys_putchar()`. `wstatus=0 rstatus=0` — `sys_blkwrite`
then `sys_blkread` both succeed. `rbuf0=42424 rbuf1=12121` (octal,
matching `printf`'s `%o`) — the exact markers the test wrote into
`sysbuf` before the syscall round trip, read back correctly after a
real disk write+read through `blockdev_write`/`blockdev_read`/
`dsk_write_block`/`dsk_read_block`. This is the whole ABI, exercised
for real: a compiled C program, using only `ulib.h`'s wrappers (no
inline asm in `syscall_test.c` at all), driving real console and real
block-device I/O through a real `SYC` trap and a real kernel-side
dispatcher.

### An incidental, honest finding from this same run: `SYC` from supervisor mode

This test never enables `Usermap` — it runs entirely in ordinary
supervisor-mode context. Tracing it (`d debug 100003`) turned up
something worth recording precisely, since `TRAP_NOTES.md`'s own
"What's open" section explicitly flagged *"a trap taken while already
in supervisor mode... not tested"*: the **first** `SYC 1,1` in the trace
carries no `A`/`B` prefix (supervisor mode, as expected), but the
**second** one does (`A001063 ... SYC 1,1`) — `__syscall_handler`'s own
bit-0-forcing reconstruction (step 5 above) doesn't distinguish "trap
came from real user mode" from "trap came from supervisor mode"; it
unconditionally sets bit 0 on return, which — confirmed directly in
`eclipse_cpu.c`'s `POPB` — re-enables `Usermap` regardless. This did
**not** corrupt anything in this specific run only because
`~/dev/simh-src/NOVA/eclipse_cpu.c`'s own one-time `MapInit` step
(`~line 728`, confirmed by reading it directly) initializes **every**
map slot to a full identity mapping (`Map[ctx][page] = page` for all 6
non-supervisor contexts, all 32 pages) the first time the CPU's main
loop runs, before this program's own code does anything — so the
"stray" `Usermap=1` this test's own handler leaves behind after its
first syscall happens to alias perfectly back onto ordinary physical
addressing, for every subsequent instruction, purely because nothing
ever loaded a non-identity map into slot 1 (User A) in this test. **This
is a real, previously-undocumented-in-this-project finding about the
simulator's own default state** (worth citing directly:
`eclipse_cpu.c` `~line 728-735`, guarded by a `MapInit` flag,
unconditional, not something any prior probe in this project needed to
know about since every earlier user-mode probe always explicitly loaded
its own map before ever relying on it). It is **not** a general safety
property of this ABI: this handler is, by design, scoped to callers
that trap from genuine user mode (the scheduler's own processes, this
document's actual target scenario) — a supervisor-mode caller under a
*non*-identity map (e.g. `scheduler_probe.s`'s own map A/B, which are
*not* identity for logical page 2) would be left running translated,
unintentionally, after its first `SYC`. Not fixed here (this ABI's
scope is user-mode callers), named honestly instead.

## 3. The verified race: `SYC` vs. a live scheduler timer

### The exact vulnerable window, re-derived from `eclipse_cpu.c`'s `Inhibit` mechanics directly

`TRAP_NOTES.md`'s own closing section speculated `SYC`'s `Inhibit = 3`
*"blocks the interrupt check for... more than one instruction right
after the trap, covering at least the handler's first instruction"* —
named as an untested expectation, not a verified one. Re-derived here
by reading the real dispatch loop (`eclipse_cpu.c` `~line 783-839`),
not assumed:

```c
if (int_req > INT_PENDING && !Inhibit) { ...take interrupt... }
...
if (Inhibit != 0) {
    if (Inhibit == 3) Inhibit = 4;      /* SYC sets Inhibit=3 as ITS OWN
                                            dispatch's last act, in the
                                            SAME iteration this runs */
    if (Inhibit == 4) Inhibit = 0;
}
```
Both blocks run once per *instruction* (one pass through the main
`while` loop), the pending-check strictly before the step. Tracing the
real iteration sequence (`K` = the `SYC` instruction itself):

| iteration | interrupt check sees | then this instruction runs |
|---|---|---|
| K   | Inhibit=0 (unchanged so far) | `SYC` itself; sets `Inhibit=3` at its own end |
| K+1 | Inhibit=3 -> **blocked** | handler instr #1 (`ESTA 0,__sysh_reason`); `Inhibit` steps 3->4 |
| K+2 | Inhibit=4 -> **blocked** | handler instr #2 (`ESTA 1,__sysh_arg1`); `Inhibit` steps 4->0 |
| K+3 | Inhibit=0 -> **not blocked** | handler instr #3 (`ESTA 2,__sysh_arg2`) -- OR the interrupt, instead |

**Exactly the handler's first two instructions are protected** — not
"the first instruction," and not the whole handler. In
`__syscall_handler`, instruction #3 (`ESTA 2,__sysh_arg2`) onward,
`DIA 0,MAP` (#4) included, is fully exposed. Worse, and more consequential
than the narrow pre-`DIA` window alone: `MapStat`'s bit 0 stays **0**
for the *entire* duration of a syscall — `SYC` clears it, and nothing
sets it again until the handler's own final `DOA`, right before `POPB`.
So a PIT interrupt landing **anywhere** during a syscall's execution —
not just the 1-2 instructions before `DIA`, but through the entire
`EJSR`-called `syscall_dispatch`, including any polling loop a real
handler like `dsk_read_block`'s might run — sees the same signature.

### Why that signature breaks `scheduler_probe.s`'s own process-identification logic

`INTHANDLER`'s `was_A`/`was_B` test (`scheduler_probe.s`, unmodified,
reused verbatim in this document's probe) is a single equality check:
`cur_mapstat == 5` (`TCB_MAPSTAT_B`) selects `was_B`; **anything else**
falls through to `was_A` — a correct, complete binary test *only* under
the precondition that an interrupt always finds `MapStat`'s bit 0 set
(the ordinary case `scheduler_probe.s` alone was ever exercised
against, since it never coexisted with `SYC`). `DIA`'s bit-0 OR-in comes
from `MapIntMode`, captured fresh at *this* interrupt's own dispatch
(`MapIntMode = MapStat` — the live value, at the moment of interrupt) —
so mid-syscall, `cur_mapstat` reads as **0** (process A: bits 1-15 all
clear) or **4** (process B: bit 13 set) — **never** 1 or 5. Both fall
through to `was_A`, unconditionally, regardless of which process was
actually interrupted.

### The probe: `examples/syscall_sched_race_probe.s`

Combines, unmodified in mechanism: `scheduler_probe.s`'s exact TCB
layout, map A/B setup, and `INTHANDLER` (copied verbatim — this probe
does not change the scheduler's own logic at all); `syscall_entry.s`'s
exact `__syscall_handler` (copied verbatim); and a hand-written
`syscall_dispatch` stand-in matching the real `SAVE`/`RTN` calling
convention exactly (reads its one pushed argument at `FP-5`, records a
call count, returns it) — so the real `EJSR`/push/pop mechanics are
exercised identically to the real, compiled `syscall_dispatch`, without
needing the whole compiled-C pipeline inside a hand-assembled race
probe. `user_loop` is modified from `scheduler_probe.s`'s own version to
add one `SYC` call per iteration, passing the live running-sum/step
registers straight through as `arg1`/`arg2` (unclobbered by design — see
the probe's own header comment on why that's correct); word 0 of the
per-process marker block is repurposed to record the syscall's own
return value, since `SYC`/`POPB` legitimately overwrites `AC0` every
round trip.

**Two real bugs found and fixed while bringing this probe up** (both
honestly worth naming, not smoothed over — this project's standing
practice):
- This probe's code (scheduler + syscall handler + dispatch stand-in
  together) is longer than `scheduler_probe.s`'s own code, so
  `scheduler_probe.s`'s own `org 0500` data-section boundary — safe for
  *its* code — left too little room here. The very first `SYC`'s own
  5-word return-block push (to `0401`-`0405`, immediately above
  `spval=0400`) silently overwrote part of `__syscall_handler`'s own
  instructions, corrupting them into a stray `JMP 52` and looping
  forever. Root-caused by single-stepping to the exact point of
  divergence (`examine`d address `0401` read back the correctly
  *deposited* `146070` before `SYC` ran, and a corrupted `000052` right
  after) — fixed by dropping the explicit `org 0500` jump (letting data
  simply follow the last instruction) and raising `spval` to `03000`,
  comfortably clear of this file's real (larger) code+data footprint.
- **A genuine SIMH-environment tracing quirk, not a bug in this
  project's own code**: `d debug 100003` + `step N` reliably produces
  full per-instruction trace output **only when the run reaches a real
  stop condition** (a `HALT`, in every case checked) — a run that ends
  via SIMH's own step-budget exhaustion (`"Step expired"`) produces
  **zero** trace lines, confirmed even for `examples/trap_probe.s`
  itself (`step 5`, stopping mid-program: 0 trace lines; `step 60`,
  reaching its own `HALT`: 49 trace lines, byte-identical to
  `TRAP_NOTES.md`'s own documented transcript). This reproduces for
  `examples/scheduler_probe.s` and `examples/pit_timer_probe.s`
  unmodified too — both infinite loops, both showing 0 trace lines
  under `step N; quit` regardless of `N`, in this environment, right
  now. Whether this differs from the environment `PROCESS_NOTES.md`'s
  own transcripts were originally captured against isn't something this
  document can determine (no earlier environment to compare against
  survives) — flagged here as a real, reproducible observation, not
  papered over. Worked around, not by chasing the tracing subsystem
  further, but by adding a deliberate kill-switch `HALT` to this
  probe's own `syscall_dispatch` stand-in (fires once a call counter
  reaches a chosen value) so a session's *final* `step N` reaches a
  genuine stop and flushes the *entire* accumulated trace (covering
  every earlier `step`/`dep PIT` command in the same session) — and,
  for the individual injection points themselves, falling back to the
  same rigorous `step 1` + `examine PC`/`examine <addr>` methodology
  `MMPU_NOTES.md`'s own evidence tables already use throughout, which
  worked reliably in every run in this section.

### Experiment 1: inject exactly in the exposed window (before `DIA`)

```
$ dgasm -t eclipse_s140 -f simh -o race_probe.simh examples/syscall_sched_race_probe.s
$ { cat race_probe.simh; echo 'dep PC 50'; echo 'step 50'; echo 'e PC'; \
    echo 'dep PIT DONE 1'; echo 'dep PIT INT 1'; \
    echo 'step 1'; echo 'e PC'; echo 'quit'; } | ~/dev/simh-src/BIN/eclipse

PC:  00403        -- ESTA 2,521 (__sysh_arg2) about to run: reason and
                     arg1 already captured (the two SYC-protected
                     instructions have run), DIA has NOT run yet --
                     exactly the predicted exposed window.
PC:  00132        -- DIA 0,MAP: INTHANDLER's OWN second instruction.
                     The single `step 1` after the deposit landed
                     directly inside the SCHEDULER's interrupt handler
                     -- __syscall_handler's own ESTA 2/DIA never ran.
```
Confirms the window is real and reachable with a single deterministic
`step`+`deposit`+`step`, exactly as the `Inhibit` analysis above
predicts — not a hypothetical.

Continuing to let `INTHANDLER` run to completion and examining the
result:
```
512 (cur_mapstat):    000000   -- bit 0 = 0 (mid-syscall), bit 13 = 0
                                  (process A) -- the ambiguous signature
534 (tcbA_pc):         000403  -- saved "resume point" for process A is
                                  now an address INSIDE __syscall_handler's
                                  own code, not inside user_loop
535 (tcbA_mapstat):     000000  -- User Enable bit OFF -- not a wrong
                                  map, no map enforcement at all if this
                                  were ever DOA'd back and POPB'd/EJMP'd
                                  naively, the same failure class
                                  syc_intmode_probe.s's own "naive" call
                                  site already demonstrated for a
                                  different reason
```
Direct, `examine`-level confirmation of the predicted corruption:
`cur_mapstat` really does read as neither `1` nor `5`, and
`INTHANDLER`'s save step really does commit a syscall-handler-internal
address and a disabled `MapStat` into a process's own TCB.

### Experiment 2: the same race, this time hitting process B's own syscall (the case that unambiguously breaks, not just looks alarming)

Experiment 1 alone, followed through further, turned up something
important enough to test in isolation and worth reporting precisely
rather than glossing over: in that specific run, letting the scheduler
subsequently resume "A" from the corrupted TCB above did **not**
immediately crash — the corrupted `tcbA_pc` (`0403`) happens to point
back inside `__syscall_handler`'s own code, at exactly the point the
race interrupted; re-entering there re-executes the handler's own
remaining steps (`ESTA 2`, `DIA`, the real `syscall_dispatch` call,
`MapStat` reconstruction, `POPB`), and — because the *original* `SYC`'s
own hardware-pushed return address was still sitting untouched on the
stack the whole time, and because the reconstruction step
unconditionally forces bit 0 on while bit 13 (A/B select) happened to
still read correctly for the process that was ACTUALLY running when
this second pass executed — this specific single-injection scenario
self-corrects: the pending syscall completes late, `POPB` resumes at
the right place, and process A's own running-sum invariant matched its
hand-predicted value exactly at the end of that run. **This is real and
was directly observed — but it is not a designed safety property**, and
generalizing "it self-healed once" into "this is safe" would be exactly
the kind of unverified extrapolation this project's own standing rule
warns against. So a second, more targeted experiment was run
specifically to find the case that does **not** self-correct:

```
$ { cat race_probe.simh; echo 'dep PC 50'; echo 'step 46'; \
    echo 'dep PIT DONE 1'; echo 'dep PIT INT 1'; \
    echo 'step 67'; echo 'e PC'; \
    echo 'dep PIT DONE 1'; echo 'dep PIT INT 1'; \
    echo 'step 1'; echo 'e PC'; echo 'e 512'; echo 'quit'; } | ~/dev/simh-src/BIN/eclipse
```
First injection (`step 46`, before A's own `SYC`) is an *ordinary*
preemption — A is correctly saved and switched out for B, exactly
`scheduler_probe.s`'s own already-verified baseline behavior, confirmed
unchanged here too. B then runs its own loop iteration fresh and reaches
its own `SYC`; stepping precisely to the equivalent exposed window
(`PC: 00403`, same code, now genuinely process **B**'s trap this time)
and injecting the second interrupt there:
```
PC:  00403         -- process B's OWN syscall, exposed window
PC:  00132         -- INTHANDLER again
512 (cur_mapstat):  000004   -- bit 13 = 1 (process B's real identity),
                               bit 0 = 0 (mid-syscall) -- 4, not 5
```
Letting `INTHANDLER` run to completion and examining every TCB field
directly:
```
530 (tcbA_ac0):    000052    -- the shared "reason" constant (42) --
                              meaningless per-process, expected
531 (tcbA_ac1):    013567    -- 6007 decimal == process B's OWN running
                              sum (seed 6000 + step 7 -- IMPOSSIBLE for
                              process A, whose step is always 3)
532 (tcbA_ac2):    000007    -- 7 == process B's OWN step constant, not
                              A's (3)
534 (tcbA_pc):     000403    -- inside __syscall_handler again
535 (tcbA_mapstat):000004    -- process B's raw bits, stored under the
                              "A" TCB slot

536 (tcbB_ac0):    054321    -- process B's id, UNCHANGED since before
                              this whole event
537 (tcbB_ac1):    013560    -- 6000 decimal -- B's ORIGINAL SEED, not
                              its real, current running sum (6007) --
                              B's actual progress this iteration is
                              GONE, never written to its own TCB
```
This is the unambiguous, non-self-correcting case: the scheduler's
`was_A`/`was_B` misidentification (`cur_mapstat=4 != 5` -> `was_A`)
overwrites **process A's own previously-valid, correctly-saved TCB**
(from the *first*, ordinary preemption) with **process B's mid-syscall
register state** — a direct, `examine`-confirmed loss of process A's
real saved context, plus process B's own TCB is left stale (never
updated to reflect its actual progress). Whatever the scheduler
subsequently does with a "resume A" using this TCB no longer
corresponds to either process's real, intended continuation — this is
real, demonstrated state corruption, not a hypothetical failure mode
inferred from reading the dispatch code alone.

### Honest summary of what this composition does and doesn't survive

- **A PIT interrupt landing entirely outside any syscall** (ordinary
  preemption, either process, anywhere in `user_loop`'s own code):
  works exactly as `PROCESS_NOTES.md` already verified — confirmed
  again here, unchanged, as the first injection in Experiment 2 above
  (A's context correctly saved, B correctly resumed from cold-boot
  state).
- **A PIT interrupt landing during any part of any syscall's
  execution** (not just the narrow 1-2-instruction pre-`DIA` window —
  the *entire* duration, since `MapStat` bit 0 stays 0 throughout):
  `cur_mapstat` reads as an ambiguous, non-`1`/non-`5` value, and
  `scheduler_probe.s`'s existing `was_A`/`was_B` test — a correct,
  complete test under its own original precondition, now violated by a
  vector it was never built to expect — **always** defaults to
  `was_A`, regardless of which process actually trapped.
  - If the process that *actually* trapped is the one already labeled
    "A" in the scheduler's own bookkeeping, this can (as Experiment 1
    showed, in that specific single-injection case) produce output that
    still looks correct, via a real but fragile and non-designed
    self-correction — depending on the corrupted resume PC landing
    somewhere inside the shared handler code where continuing is
    semantically safe, and on the original `SYC`'s own stack-pushed
    return address never having been disturbed by anything else in the
    meantime.
  - If the process that actually trapped is the *other* one (Experiment
    2), the result is unambiguous corruption: the wrongly-identified
    process's real, valid saved state is overwritten, and the correctly
    -identified-but-wrongly-labeled process's own progress is lost.

## 4. What remains open

- **This document verifies the failure mode directly; it does not
  attempt a fix.** A real fix needs the syscall handler (or the
  scheduler's own `INTHANDLER`) to communicate "a syscall is currently
  in flight, for whichever process, don't trust `MapStat` bit 0 for
  identification right now" through some channel neither currently
  provides — e.g. a shared "syscall in progress" flag the scheduler's
  own dispatch checks before trusting `cur_mapstat`, falling back to
  some other identification path (there is no obvious hardware-only
  one; `TRAP_NOTES.md` already established `SYC` has no `MapIntMode`-
  style save of its own). Scoped out here, matching this project's own
  practice of naming a real next step precisely rather than rushing a
  fragile version of it into this increment.
- **Only PIT was raced against `SYC`, and only via deterministic SCP
  deposit injection**, not the *real*, self-arming PIT timer under
  genuine wall-clock non-determinism. `PROCESS_NOTES.md`'s own section 1
  already established that the real timer's calibration makes hitting a
  1-2-instruction window this way statistically implausible to
  reproduce on demand — the deterministic injection technique exists
  specifically because of that, and is this project's own established
  tool for exactly this class of question (`MMPU_NOTES.md`'s
  `mmpu_intmode_resume_probe.s`). This probe's own `_start` still arms
  the real PIT (unused for the actual race verification, present for
  realism) — a longer, real-timer run of this same probe (many millions
  of steps, matching `pit_timer_probe.s`'s own scale) was **not**
  additionally run to see how often the real timer happens to land
  inside a syscall's window in practice; the deterministic result above
  already answers "can this happen and is it safe" (yes it can, no it
  isn't), which is the question that matters, but a real-timer run
  would add a frequency estimate this document doesn't have.
- **Only the two specific injection points in Experiments 1 and 2 were
  tested** (pre-`DIA`, and — for the second process — the equivalent
  point). Landing an interrupt deeper inside `syscall_dispatch` itself,
  or inside a real (not stand-in) block-device syscall's own polling
  loop (`dsk_read_block`'s `SKPDN`-based wait, a much longer real
  window), was not separately exercised — expected, from the "`MapStat`
  bit 0 stays 0 for the syscall's entire duration" finding, to exhibit
  the identical `was_A`-defaulting failure mode, but not directly
  confirmed at those specific points.
- **Nested/re-entrant syscalls** (a second `SYC` executed while a
  syscall from either process is still "in flight," e.g. via the
  self-correcting detour Experiment 1 found) were not deliberately
  constructed — `__syscall_handler`'s own scratch cells
  (`__sysh_reason`/`arg1`/`arg2`/`mapstat_raw`, `sysarg1`/`sysarg2`) are
  plain shared globals, not per-process or re-entrancy-safe, so a
  second syscall overlapping a first one's own not-yet-reconstructed
  window is a real, structurally-evident additional collision risk
  (demonstrated to be reachable, not just imagined, by Experiment 1's
  own "resumed back into the middle of the handler" outcome) — not
  separately confirmed to corrupt anything beyond what Experiments 1/2
  already show.
- **`SYC` from supervisor mode, under a non-identity map**: section 2's
  incidental finding (this ABI's handler always re-enables `Usermap` on
  return, correct only for a trap that genuinely came from user mode)
  was observed but not exercised against a real non-identity map — see
  that section for the precise, honest scope of what was and wasn't
  confirmed there.

## 5. What this syscall ABI does not attempt

- **A general user-buffer copy for block I/O.** `SYS_BLKREAD`/
  `SYS_BLKWRITE` transfer through a fixed kernel-side buffer
  (`sysbuf`), not a user-supplied pointer — a real copy would need to
  translate a user-mode logical address through *that process's own*
  live map, which is exactly the map that's live-and-unsaved the
  instant `SYC` first traps, and no longer even selected by the time
  the handler runs (`SYC`'s own dispatch unconditionally disables
  `Usermap`). Not attempted here; `examples/mmpu.c`'s
  `mmpu_read_far`/`mmpu_write_far` are the natural building block a
  future version of this would need, once combined with a way to know
  which of the two live maps a given syscall's caller was using (itself
  exactly this document's own open race, worth resolving first).
- **Process identification for a syscall handler that doesn't rely on
  `MapStat`.** See "What remains open" above.
- **`open`/`close`/file-descriptor semantics, `fork`/`exec`, or any
  process-lifecycle syscalls.** This ABI's four syscalls are the
  minimal real set the task asked for (console I/O, block I/O),
  backed by already-verified kernel code — not a general POSIX-shaped
  surface.

## 6. Regression check

Only new files added (`examples/syscall.h`, `examples/syscall.c`,
`examples/syscall_entry.s`, `examples/ulib.h`, `examples/ulib.c`,
`examples/syscall_test.c`, `examples/syscall_sched_race_probe.s`, this
document) — `examples/scheduler_probe.s`, `examples/proc.h`,
`examples/console.h`/`.c`, `examples/blockdev.h`/`.c`,
`examples/dsk.h`/`.c` are all byte-for-byte unmodified (`git diff
--stat` against each is empty). `dgasm-src` CTest, re-run after these
additions: **315 tests, 314 passing, 1 Not Run** (`memcheck_hello`,
valgrind still not installed) — identical to every prior phase's
baseline. `examples/syscall_test.c`'s own full run (section 2) was
re-confirmed a second time, byte-identical, immediately before writing
this document.
