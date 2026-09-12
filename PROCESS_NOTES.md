# PROCESS (preemptive 2-process round-robin scheduler) — design and verification

Synthesizes three prior, independently hand-verified results into this
project's first real OS-design increment — a process abstraction and a
scheduler, not another single hardware primitive:

- `MMPU_NOTES.md`'s "Phase 2: `MapIntMode`" section, whose closing finding
  is the load-bearing precondition for everything here: preempting
  user-mode MMPU-active code via a real interrupt is safe *if and only
  if* the handler's first action is `DIA 0,MAP` before any other
  MAP-device access. That section stopped at "here's the contract, not
  yet a scheduler built on it" — this document is that next step.
- `TRAP_NOTES.md`'s `SYC` mechanism — used here only as a boundary to
  respect, not a tool: this scheduler is pure *involuntary* preemption,
  and deliberately never issues `SYC`. See "Syscalls and this scheduler"
  below for why that boundary matters and how a future syscall would fit
  without breaking it.
- `PMM_NOTES.md`'s physical frame allocator and its own already-flagged
  open question: *"any physical frame [the allocator] hands to a process
  is only reachable through one of exactly two live user maps at a
  time... multiplexing more than a couple of live 'big' address spaces
  means swapping user-map contents, not just picking a free frame."* This
  document's scope is deliberately bounded to the N=2 case that hardware
  gives you directly, precisely because that swapping problem is real,
  separate, harder work — see "Future work" below.

**Bottom line, stated up front**: **yes** — a real, preemptive,
round-robin scheduler between 2 concurrent user-mode processes (one in
user map A, one in user map B), switched by a genuine, self-arming,
periodic hardware timer interrupt (not cooperative yielding, not
SCP-console interrupt injection), is built and verified below with real
single-stepped/traced SIMH evidence. Both processes visibly advance,
survive thousands of real preemptions with zero register or address-space
corruption, and resume at the *exact* interrupted instruction — down to
sub-loop-iteration granularity, confirmed by trace, not asserted.

## 1. The timer: a real, self-arming, periodic interrupt

### The debugging tool this is NOT

`MMPU_NOTES.md`'s `MapIntMode` section used `deposit PIT DONE 1` +
`deposit PIT INT 1` to inject a deterministic interrupt from the SCP
console, between `step` calls. That is real and useful — it is how this
project gets single-instruction-precise interrupt timing for testing —
but it pokes SIMH's own register state from *outside* the running
program. A real kernel cannot issue an SCP `deposit` to itself. This
increment needed the actual hardware-documented way supervisor-level code
arms its own periodic timer.

### The real mechanism, read from `eclipse_cpu.c`'s PIT device (~line
5815-5881) and confirmed by the manual's own `PIT`/device-code table

- `DEV_PIT` is device code `043` octal (`nova_defs.h:214`).
- `DOA ac,PIT` sets `pit_initial = AC` and calibrates the timer's period
  (`sim_rtcn_init(pit_time, 1)`).
- `NIOS PIT` (pulse S, "start"): `pit_counter = pit_initial;`, sets
  `dev_busy`, and — if the unit isn't already running —
  `sim_activate`s the first tick.
- `pit_svc` (the real per-tick handler, wall-clock-calibrated via
  `sim_rtcn_calb` exactly like every other timed SIMH device this project
  has touched) does two things every tick, **unconditionally**:
  `pit_counter++`, then **reschedules itself again** (`sim_activate
  (&pit_unit, t)`) regardless of anything else. Only when `pit_counter`
  wraps past `0177777` does it actually set `dev_done`/`int_req` (a real
  CPU interrupt) and reload `pit_counter = pit_initial`.

Net effect, read directly from source before ever running anything: a
**single** `DOA` (load a small initial count) + `NIOS PIT` produces a
real, self-perpetuating, periodic interrupt source, with **zero** further
software or console involvement required to keep it ticking. This is
categorically different from the SCP-deposit technique — this is the
timer genuinely running inside the simulated hardware's own event queue.

### Empirical confirmation (`examples/pit_timer_probe.s`)

A plain supervisor-mode "heartbeat" loop (`INC 1,1` / `JMP heartbeat`) —
no MMPU involvement, isolating the timer mechanism on its own before
combining it with anything else — arms the PIT with initial count
`0177770` (wraps, i.e. fires, after only 8 ticks) and installs a handler
at location 1 (the generic device-interrupt vector) that snapshots the
heartbeat counter, acks (`DIB ac,077`), counts firings, and — the real
finding this probe exists to nail down — **clears and re-arms the PIT
every single time**:

```
$ dgasm -t eclipse_s140 -f simh -o pit_timer_probe.simh pit_timer_probe.s
$ { cat pit_timer_probe.simh; echo 'dep PC 50'; echo 'step 21001010'; \
    echo 'e 102'; echo 'e 103'; echo 'quit'; } | eclipse

102:	015731      -- fire_count: 6873 decimal, real interrupts, self-armed
103:	106314      -- last_ac1: the heartbeat's value at the MOST RECENT
                     interrupt -- large and non-repeating across
                     inspections, confirming each interrupt lands at a
                     genuinely different point in the loop, not a fixed one
```

**A real, non-obvious requirement found and confirmed empirically, not
assumed**: the *only* PIT operation that clears a pending Done/interrupt
condition is `NIOC PIT` (pulse C) — but `eclipse_cpu.c`'s own `iopC`
handler for PIT **also cancels the ticking unit**
(`sim_cancel(&pit_unit)`). A handler that clears without re-arming gets
exactly **one** interrupt, ever. The fix — confirmed necessary by first
running a version without it, which produced `fire_count=1` after
millions of steps, then adding it — is the same "clear, then restart"
idiom `examples/isr_c_test.c`'s own handler already uses for the RTC/CLK
device (`NIOC 011` then `NIOS 014`): `NIOC PIT` immediately followed by
`NIOS PIT`, every tick.

**A real dead end investigated and worked around, not ignored**: the
first attempt at this probe used `step 2000000` and then `step 50000000`
and saw **zero** firings in either case — not a bug, a genuine
calibration effect. Direct register examination (`examine PIT COUNT`)
across intermediate step counts showed why: `pit_counter` really was
advancing, but the *rate* was collapsing (2 ticks in the first 1000
steps, only 4 more ticks over the next 1,000,000) — `sim_rtcn_calb`
inflating its own per-tick instruction-count estimate because batch
`step` execution is vastly faster than the wall-clock rate (`pit_tps =
10000` ticks/sec) it is trying to track. Pushing the step count up to
21,001,010 (chosen empirically, not derived) got past this and into a
regime where the timer fired thousands of times. This is the same
wall-clock-calibration property `MMPU_NOTES.md`'s earlier `MapIntMode`
investigation already found for the RTC/CLK device, now confirmed to
extend to PIT's *real* self-arming path specifically (as opposed to the
SCP-deposit path, which sidesteps it entirely — the actual reason that
technique remains useful as a *testing* tool even though it is not how a
real kernel arms its own timer).

**Determinism, checked, not assumed either way**: running the identical
probe twice at the identical step count:
```
run 1: 102: 015731   103: 106314
run 2: 102: 015726   103: 114715
```
Close in magnitude, not bit-identical — real wall-clock-calibration
non-determinism, exactly as expected from the mechanism above. This is
the concrete reason `scheduler_probe.s`'s own verification (below) is
invariant-based rather than pinned to an exact instruction count the way
`mmpu_intmode_resume_probe.s`'s SCP-deposit-based test could be.

## 2. Why ION is not enabled until inside the resume path

A real correctness question, worked out from source before writing any
scheduler code: if the PIT is armed early (during `_start`'s map-loading
setup, before either process has actually started running), could a
stray interrupt land with no real process context yet established?

Confirmed directly in `nova_defs.h`: `INT_PENDING = INT_ION +
INT_NO_ION_PENDING` (bits 19 and 18), while every device interrupt bit —
PIT included — lives at bit 17 or below (`INT_DEV = (1<<INT_V_STK)-1`).
The main loop's *only* interrupt-dispatch gate is `int_req > INT_PENDING`.
Algebraically, with bit 19 (ION) clear, `int_req`'s maximum possible value
(all device bits plus the no-ion-pending bit) is `262144 + 131071 =
393215`, strictly less than `INT_PENDING = 786432`. **A pending device
interrupt cannot be delivered while ION is off — not "unlikely," provably
never**, for as long as this arithmetic relationship holds (i.e., for as
long as no device bit set encodes a value at or above bit 18, which is
true for every device this project uses).

This lets `scheduler_probe.s`'s `_start` arm the PIT as early as
convenient with zero risk, and lets the scheduler defer `NIOS 077` (ION)
entirely until the *last* instruction of `resume_A`/`resume_B`,
immediately before the indirect return jump — by which point a real
process's MapStat and registers are already fully live. The 1-instruction
ION-enable delay already established by every prior interrupt probe in
this project (`isr_c_test.c`, `mmpu_intmode_probe.s`) then guarantees the
earliest a real interrupt can land is inside the resumed user process
itself, never mid-dispatch.

## 3. The TCB (`examples/proc.h`)

```
offset  name       meaning
0       ac0        AC0 at last preemption
1       ac1        AC1 ("running sum" register, this design's choice)
2       ac2        AC2 ("step constant" register)
3       ac3        AC3 ("iteration count" register)
4       pc         resume PC (low 15 bits) with carry folded into bit 15
5       mapstat    the exact DOA-loadable MapStat word for this process
                    (1 = User A, 5 = User B)
```

**Why only 2 TCBs, hardcoded, not a generic N-slot table**: the real
S/140 MMPU has exactly two user address map slots (`MMPU_NOTES.md`'s
Phase 1 section, confirmed against the manual's own page images: *"The
MMPU can hold two user maps, but only one can be enabled at any one
time,"* and `DOA`'s own 3-bit Map Select field marking every other code
as hardware-"Reserved"). A TCB here is permanently bound to one of
exactly two hardware slots, not a generic virtual address space — there
is nothing to index by beyond "A or B." `examples/scheduler_probe.s`
hand-unrolls the 2-way save/restore/dispatch the same way
`mmpu_intmode_resume_probe.s`'s naive/informed paths and
`trap_probe.s`'s `path_a`/`path_b` dispatch already do, rather than
building pointer-indexed TCB-table machinery for a fixed cardinality of
2.

**Determining WHICH process was interrupted, from hardware, not software
bookkeeping** — this was an explicit design goal, not an incidental
choice: `DIA`'s `MapIntMode`-informed readback reproduces the
pre-interrupt `MapStat` bit-for-bit (only bit 0 is ever cleared by the
dispatch; bit 13, the A/B select, survives untouched —
`mmpu_intmode_resume_probe.s` already proved this directly, and this
increment relies on that finding rather than re-deriving it). Since this
scheduler only ever `DOA`s exactly two constants (1 for A, 5 for B), one
equality test — `SUB# 1,0,SZR` with AC1=5 — cleanly distinguishes them
with no bitmasking needed. Confirmed live in trace (below): each
interrupt's `DIA` readback is exactly `1` or `5`, matching whichever
process was actually running, every single time observed.

**Carry**: genuinely not saved by hardware for this interrupt path. The
5-word return-block auto-push that folds carry into bit 15 (`MMPU_NOTES.md`'s
page-fault section, `TRAP_NOTES.md`'s `SYC` section) is unique to the
fault and `SYC` vectors. A plain device interrupt (location 1, what this
scheduler uses) does only `M[0] = PC` — no register push, no carry
capture — confirmed directly in `eclipse_cpu.c`'s generic
interrupt-dispatch block. This handler captures it by hand:

- **Capture**: `MOV# 0,0,SZC` — the no-load (`#`) bit (dgasm's `#`
  suffix, encoding bit `0x8`, confirmed in `opcode.c`'s
  `encode_alu_instruction`) skips *both* the accumulator write and the
  `C = src&0200000` write (`eclipse_cpu.c`'s `if ((IR&010)==0) {...}`
  guards both together) — so this is a genuinely non-destructive test,
  safe to run before AC0's true value or C's true value have been saved
  anywhere else. The result (0 or `0100000`) is folded into the saved PC
  exactly the way hardware's own return block does it (`ADD`, safe as OR
  here since a real PC value never has bit 15 set).
- **Restore**: `MOVZ 0,0` forces C=0; `MOVO 0,0` forces C=1. Both
  confirmed by hand from the ALU formula (`eclipse_cpu.c`'s operate-
  instruction switch) to leave the accumulator's own 16-bit value
  completely unchanged — the carry-in mode (`Z`/`O`) only ever affects
  the extra bit-16 carry slot a self-`MOV` passes straight through
  unmodified in bits 0-15. This means the restore sequence can safely use
  AC0 as scratch immediately before AC0's *real* saved value is reloaded
  (last, since `LDA` never touches C).

**Honest limitation, not smoothed over**: `user_loop`'s own arithmetic
(`ADD 2,1`, not an `ADC`-chained multi-word computation) never actually
*depends* on carry-in from a previous iteration. Carry save/restore is
implemented for TCB completeness and structural correctness (matching
the task's own "AC0-AC3/PC/carry as needed for a full context" spec, and
because a real kernel supporting arbitrary user code cannot assume carry
never matters), but a broken carry restore would not be visible in this
probe's own verification output the way a broken AC1/AC2 restore would
be. See "What this doesn't prove" below.

## 4. Both "processes," and why a wrong restore would be visible

Both processes run the **same physical code** (`user_loop`) — this is
real, not a simplification made for convenience: logical page 0 is
identity-mapped in *both* maps (`mmpu_usermode_probe.s`'s established
safety pattern), so the code itself is map-independent. Everything that
makes process A and process B behave differently lives entirely in their
register state, which is exactly what a TCB is for.

Each iteration:
```
INC 3,3            ; AC3 (iteration count) += 1
ADD 2,1            ; AC1 (running sum) += AC2 (fixed per-process step)
ESTA 0, 04200, 0    ; word0: id marker (must stay constant, forever)
ESTA 1, 04201, 0    ; word1: running sum
ESTA 2, 04202, 0    ; word2: step (must stay constant, forever)
ESTA 3, 04203, 0    ; word3: iteration count
JMP user_loop
```
translated through logical page 2 to a distinct physical page per map —
reusing `mmpu_context_switch_probe.s`'s own already-verified targets
unchanged (map A → phys page `0150` octal, map B → phys page `0044`
octal), on the same reasoning that document already established (small,
known-safe, under the console's 128K-word `awidth` examine ceiling).

- Process A: id=`012345` octal, seed(AC1)=2000 decimal, step(AC2)=3
- Process B: id=`054321` octal, seed(AC1)=6000 decimal, step(AC2)=7

**The invariant this makes checkable, worked out by hand before treating
any run's output as correct** — this project's standing "falsifiable
prediction, then confirm" discipline: at any point,
```
running_sum == seed + step * iteration_count          (Case 1: ADD already ran for this iteration)
running_sum == seed + step * (iteration_count - 1)     (Case 2: interrupted between INC and ADD)
```
Both cases are legitimate, not a fudge: the CPU can be preempted at
*any* instruction boundary, including the one between `INC 3,3` and `ADD
2,1` inside a single loop pass — a real interrupt landing there correctly
saves AC3 already incremented but AC1 not yet updated for that same
pass. Which case applies for a given snapshot is visible directly in the
trace (see below): a process that resumes at `ADD 2,1` (not at `INC 3,3`)
was interrupted in exactly that window. Neither case can be produced by
cross-process register corruption mixing in the *other* process's step
(stepA=3 and stepB=7 share no small common factor with either seed),
which is the actual failure mode this check exists to catch — the same
"resumed the wrong process" bug `mmpu_intmode_resume_probe.s`'s own naive-
resume path already demonstrated directly. The constant ID marker (AC0)
is a second, simpler, purely-bitwise check requiring no arithmetic at
all: it must read back identical to its initial value on every single
inspection, forever.

## 5. The scheduler itself (`examples/scheduler_probe.s`)

On every timer interrupt:
1. Save the true AC0 (plain `ESTA`, doesn't touch C).
2. `DIA 0,MAP` — **the first and only MAP-device access before this
   process's mapstat is safely captured**, per `MMPU_NOTES.md`'s
   `MapIntMode` contract.
3. Capture carry via the `#`-mode skip-test described above.
4. Save AC1-AC3 and reconstruct the interrupted PC (read raw physical
   location 0, fold in the captured carry bit).
5. Ack the device (`DIB ac,077`).
6. Determine which process was interrupted from `cur_mapstat` (hardware
   state, not software bookkeeping) and save the just-captured context
   into that process's TCB.
7. Ack+re-arm the PIT (`NIOC PIT` then `NIOS PIT` — see section 1).
8. Dispatch to the *other* process's resume routine — unconditional
   strict alternation is the complete scheduling policy for exactly 2
   processes; there is no "run queue" to consult because there are only
   ever two runnable entities and preemption always means "run the one
   that wasn't just running."
9. `resume_A`/`resume_B`: reprogram `MapStat` via `DOA` from the TCB's
   saved `mapstat` field, compute and stash the pure (carry-stripped)
   resume PC, restore carry, reload AC1-AC3 then AC0 last, enable ION as
   the literal final instruction, then `EJMP @resume_target` — an
   indirect extended jump, which is both the return AND the trigger that
   flips `Usermap` per the `DOA` just issued (confirmed in source: the
   memory-reference indirect-chain block used by `JMP`/`JSR`/`EJMP`
   alike checks `MapStat & 1` on every indirect fetch, the identical
   mechanism `effective()` uses for `LDA`).

## 6. Verification

### Short run: one full A→B→A round-robin cycle, traced

```
$ dgasm -t eclipse_s140 -f simh -o scheduler_probe.simh scheduler_probe.s
$ { cat scheduler_probe.simh; echo 'set debug trace.log'; echo 'd debug 100003'; \
    echo 'dep PC 50'; echo 'step 1600'; echo 'quit'; } | eclipse
```

First interrupt — lands genuinely mid-`user_loop`, contract respected,
correct process identified:
```
IA000101 acs: 012345 004363 000003 000141 0 INC 3,3
--------- Interrupt 1000004 (43) to    114 ---------
  000114 acs: 012345 004363 000003 000142 0 ESTA 0,513      <- save0 (not MAP)
  000116 acs: 012345 004363 000003 000142 0 DIA 0,MAP        <- FIRST MAP access
  000117 acs: 000001 004363 000003 000142 0 ESTA 0,521       <- cur_mapstat=1 (A)
  ...
  000154 acs: 000001 000043 000003 000142 0 ELDA 1,502       <- AC1=5
  000156 acs: 000001 000005 000003 000142 0 SUB# 1,0,SZR     <- 1-5 != 0, no skip
  000157 acs: 000001 000005 000003 000142 0 JMP 161          <- was_A taken (correct)
  ...
  000260 acs: 000001 000005 000003 000142 0 NIOC 0,43
  000261 acs: 000001 000005 000003 000142 0 NIOS 0,43        <- PIT ack+rearm
  ...
  000325 acs: 000005 000005 000003 000142 0 DOA 0,MAP
325 DOA 0=5 (Load Map Status)                                 <- resuming B
  ...
I 000361 acs: 054321 013560 000007 000000 0 EJMP @522
IB000101 acs: 054321 013560 000007 000000 0 INC 3,3          <- B running, fresh (seed=6000,step=7,iter=0)
```

Second interrupt — B is now the one preempted, correctly identified, A
resumed with its **exact** prior state:
```
IB000101 acs: 054321 015027 000007 000141 0 INC 3,3
--------- Interrupt 1000004 (43) to    114 ---------
  000116 acs: 054321 015027 000007 000142 0 DIA 0,MAP
  ...
  000154 acs: 000005 000043 000007 000142 0 ELDA 1,502       <- AC1=5
  000156 acs: 000005 000005 000007 000142 0 SUB# 1,0,SZR     <- 5-5 == 0, SKIP
  000160 acs: 000005 000005 000007 000142 0 JMP 217          <- was_B taken (correct)
  ...
  000265 acs: 000001 000005 000007 000142 0 DOA 0,MAP
265 DOA 0=1 (Load Map Status)                                 <- resuming A
  ...
  000310 acs: 000000 100000 000007 000142 0 ELDA 1,526        <- reload tcbA_ac1 = 004363
I 000321 acs: 012345 004363 000003 000142 0 EJMP @522
IA000102 acs: 012345 004363 000003 000142 0 ADD 2,1           <- resumed at ADD, not INC:
```
`004363` octal, `000003` (step), `000142` (iteration count) are **bit-for-
bit identical** to A's own register state at the exact instant it was
first interrupted (`acs: 012345 004363 000003 000142` at the very top of
this excerpt) — a direct, trace-level proof of exact-state resume, not
inferred from a final memory dump. The resume PC itself (`IA000102`, not
`IA000101`) independently confirms *where* within the loop body A was
interrupted (immediately after `INC 3,3`, before `ADD 2,1` had run) — the
"Case 2" invariant window described in section 4, visible directly rather
than deduced after the fact.

### Long run: thousands of real preemptions, invariant-checked

```
$ { cat scheduler_probe.simh; echo 'dep PC 50'; echo 'step 2000000'; \
    echo 'e 524'; echo 'e 525'; echo 'e 526'; echo 'e 527'; echo 'e 530'; \
    echo 'e 533'; echo 'e 534'; echo 'e 535'; echo 'e 536'; echo 'quit'; } | eclipse

524:  005224     -- switch_count: 2708 decimal real preemptions
525:  012345     -- tcbA id  (unchanged from boot -- no cross-process bleed)
526:  006772     -- tcbA sum  = 3578 decimal
527:  000003     -- tcbA step = 3
530:  001016     -- tcbA iter = 526 decimal
533:  054321     -- tcbB id  (unchanged from boot)
534:  023127     -- tcbB sum  = 9815 decimal
535:  000007     -- tcbB step = 7
536:  001041     -- tcbB iter = 545 decimal
```
Hand-checked, per the two-case invariant (section 4), *before* treating
either as correct:
- A: `2000 + 3*526 = 3578` — matches `006772` octal exactly (Case 1).
- B: `6000 + 7*545 = 9815` — matches `023127` octal exactly (Case 1).

Both processes' ID markers read back bit-for-bit identical to their boot
values after 2708 real, hardware-timer-driven preemptions. Re-run twice
independently at `step 3000000` (a different step count, chosen after the
fact, not tuned to produce a particular answer):
```
run 1: 524:007736  525:012345 526:010414 527:000003 530:001425
                    533:054321 534:026707 535:000007 536:001461
run 2: 524:007736  525:012345 526:010414 527:000003 530:001425
                    533:054321 534:026707 535:000007 536:001461
```
(Identical between these two particular runs — not a general determinism
guarantee, see section 1's own repeatability finding for
`pit_timer_probe.s`; both this pair of runs and that earlier pair are
reported honestly rather than cherry-picked.) Checked:
- A: iter=789 decimal. `2000+3*789=4367` → `010417` octal — does **not**
  match `010414`. Checked the Case 2 alternative: `2000+3*(789-1) =
  2000+3*788 = 4364` → `010414` octal — **matches exactly**. A was
  captured mid-pass, between `INC` and `ADD`, exactly the same window the
  short run's trace showed directly.
- B: iter=817 decimal. `6000+7*817 = 11719` → `026707` octal — matches
  `026707` exactly (Case 1).

Every value across both long runs and the fully-traced short run is
accounted for by the two-case invariant, with zero unexplained
discrepancies — the standard this project holds every claim to
(`MMPU_NOTES.md`'s own recurring "falsifiable prediction, then confirm"
rule), applied here to a scheduler operating under genuinely
non-deterministic real-timer preemption rather than a single fixed
instruction count.

### Regression check

`dgasm-src` CTest, re-run after these additions: **315 tests, 314
passing, 1 Not Run** (`memcheck_hello`, `valgrind` still not installed —
identical to every prior phase's baseline). `examples/sizeof_check.c`
through the full `eclipse-cc` pipeline: unchanged (`... 10 14`, matching
the documented tail). Zero `dgasm-src`/`eclipse-cc`/`llvm-project`/
`simh-src` changes — `examples/pit_timer_probe.s`, `examples/proc.h`,
`examples/scheduler_probe.s`, and this document are the only additions.

## 7. What this doesn't prove

- **Carry correctness under real dependence.** As flagged in section 3,
  `user_loop` never has a computation that actually depends on carry-in,
  so a broken carry restore would not surface in this probe's own
  invariant checks. The save/restore mechanism is structurally verified
  (the ALU-formula analysis in section 3 is exact, not approximate), but
  not exercised end-to-end by a carry-dependent user computation. A
  cheap future addition: an `ADC`-chained multi-word checksum in
  `user_loop`, seeded so a wrong carry visibly changes the result.
- **Nested interrupts.** This scheduler's handler runs entirely with ION
  off (see section 2) — by design, a second interrupt cannot land mid-
  handler. That is the same scoping `TRAP_NOTES.md`'s own "What's open"
  section already flagged for `SYC` and never closed; still open here for
  the identical reason (needs the same non-single-step-able tooling
  `MMPU_NOTES.md`'s `MapIntMode` investigation already flagged as a
  different, harder problem).
- **More than 2 processes.** See "Future work" below — this is a scope
  boundary, not an oversight.
- **Process creation, loading a new program image, or destruction.** Both
  TCBs and both maps are set up once, statically, at assembly time. There
  is no "spawn a process" operation here at all.
- **Any filesystem or I/O interaction from a scheduled process.**

## 8. Syscalls and this scheduler

Explicitly out of scope to implement here, per the task's own framing —
`SYC` is for voluntary syscalls, this scheduler is pure involuntary
preemption, and conflating the two would be a real design mistake, not
just an unfinished feature. But it is worth naming precisely *why* they
don't conflict and how a future syscall would need to fit:

- `SYC`'s own dispatch (`TRAP_NOTES.md`) unconditionally clears
  `MapStat`'s bit 0 and sets a 1-instruction `Inhibit` (only when the trap
  came from real user mode) — structurally similar to, but a *different*
  vector (location 2) from, this scheduler's location-1 device-interrupt
  path. A process that executes `SYC` while this scheduler's timer is
  also armed would need its syscall handler to follow the **same**
  `DIA`-first discipline this document's scheduler follows, before
  touching the MAP device for any reason — `TRAP_NOTES.md`'s own
  handler never needed this because it never coexisted with a real
  timer; this document's finding is that the requirement generalizes to
  *any* handler that might run while a process's MapStat reflects live,
  unsaved user-map state, not just the timer ISR specifically.
  - **Not yet safe as designed**: `SYC`'s dispatch does **not** save
    `MapIntMode` the way the generic interrupt path does — it clears
    `MapStat` bit 0 directly with no `MapIntMode = MapStat` step
    (confirmed in `TRAP_NOTES.md`'s own quoted source). A syscall
    handler therefore cannot recover the pre-trap A/B state via `DIA`
    the way this scheduler's timer handler does — it would need the
    calling convention itself to identify which process trapped (e.g. a
    reserved argument, or reading which map was active via some other
    already-known channel), since the hardware does not hand it back
    for this vector the way it does for the generic interrupt path.
    This is a real, previously-unflagged gap between the two vectors,
    found by this document's own comparison, not carried over from
    either prior note.
- A process's TCB (this document's `examples/proc.h`) already has
  everywhere a syscall handler would need to save state (AC0-AC3, PC,
  mapstat) — reusing the same 6-word layout for a voluntary trap's own
  save is a natural fit, not a redesign, if the `MapIntMode` gap above is
  addressed first.

## 9. Future work: N > 2 processes (map-content swapping)

**The hard hardware constraint, unchanged since `MMPU_NOTES.md`'s Phase 1
section**: the S/140 MMPU has exactly 2 real user address map slots. This
scheduler uses both of them, permanently, for exactly 2 processes. A
general "arbitrary number of processes" scheduler is not a bigger version
of this one — it needs an additional, genuinely different mechanism:
swapping full page-table *contents* into and out of the 2 physical map
slots (via `LMP`, reloading a slot's 32 logical-page entries) whenever a
process not currently resident in A or B needs to run, on top of
everything this document already does for switching *which* resident
process is active. Concretely, a future increment would need:

- A per-process saved *page table* (32 words, not just 6) in addition to
  this document's TCB — `PMM_NOTES.md`'s allocator already hands out the
  physical frames such a table would describe; nothing here yet manages
  which logical pages a non-resident process's frames should be
  reloaded into.
- A policy for *which* resident slot (A or B) to evict when a
  non-resident process needs to run — this document's strict alternation
  is a complete policy for N=2 precisely because there is no "which one"
  question; N>2 reintroduces exactly the scheduling-policy question this
  document's scope deliberately avoided.
- Cost accounting: an `LMP` reload is a real, multi-word operation, unlike
  this document's `DOA`-only resume (which touches zero page-table data).
  Whether that cost is acceptable per-switch, or whether it argues for a
  longer minimum process quantum than this document's essentially-
  unthrottled PIT period, is unexamined here.

This is real, separate, harder work — flagged as the natural next step,
not attempted in this increment, matching this project's standing
practice (`MMPU_NOTES.md`'s own "Phase 2 would actually need" list) of
naming a scoped-out next step precisely rather than either attempting a
fragile version of it or leaving it unnamed.
