# Block-device abstraction + console driver — phase 6

Builds directly on `STORAGE_NOTES.md`'s already-verified `DSK` register
sequence (`examples/disk_probe.s`, hand-assembled) and this project's
existing TTI/TTO console-I/O precedent (`~/dev/interrupt_test*.s`,
`eclipse-toolchain/rt/eclipse_rt.c`'s `putchar`/`getchar`). The goal of
this phase: a small, generic block-device interface a kernel can call
(`blockdev_read(dev, blockno, buf)` / `blockdev_write(...)`) without
caring which physical controller is behind `dev`, DSK wired in as the
one real backend, and a minimal kernel-level console driver — plus,
critically, real evidence both actually run correctly on
`~/dev/simh-src/BIN/eclipse`, not just "it compiled."

**Bottom line up front**: the generic interface works end to end, with
real verified output (below). It's a struct of function pointers — the
natural C idiom — but not a single flat struct; see "The interface
design" for a real backend limitation this work found (and worked
around, not the textbook-C shape but not a switch-based dispatch
either) along the way. The console driver works too, including
`console_getchar()`'s input path, verified with SIMH's own `SEND`
command injecting real console input.

## The interface design (`examples/blockdev.h`, `examples/blockdev.c`)

Function pointers through a struct field are real and proven on this
backend — `DEBUGGING_NOTES.md` entry #17 ("Indirect (function-pointer)
call support added") was re-read in full before designing anything
here, not trusted secondhand. Two specific claims from that entry
matter directly: calling through **a struct pointer's function-pointer
field** (`v.fptr()` reached via `dev->read_block(...)`-shaped code) is
one of entry #17's own four originally-fixed c-testsuite cases, and a
**global struct statically initialized with a function-pointer value**
(exactly `blockdev_t dsk_dev = { ... };`) was a second bug entry #17
fixed in the same pass. Both proven, both regression-tested there.

So the first design tried was the obvious one:

```c
typedef struct blockdev {
    int (*read_block)(void *ctx, unsigned int blockno, void *buf);
    int (*write_block)(void *ctx, unsigned int blockno, void *buf);
    void *ctx;
} blockdev_t;
```

This compiled cleanly through the full `eclipse-cc` pipeline. It did
**not** run correctly: `blockdev_write()` (which reads `write_block`,
the struct's *middle* field) trapped at runtime — `PC` snapped to `0`,
the exact "corrupted SAVE/RTN linkage" signature `DEBUGGING_NOTES.md`
entry #11 already documents for a different bug.

Root-caused before working around it, per this project's own standing
rule — re-read `DEBUGGING_NOTES.md` entry #15's still-**open** bug #2
first: *"a struct field reached through a pointer variable ... computes
a different physical word for a STORE than for the corresponding LOAD
of the exact same field, when the field is neither the struct's first
nor last."* That entry is explicit that this is unfixed (c-testsuite
00163/00179/00180/00205 still skip-listed for it) and that neither
entry #17's fix nor entry #18's fix closes it out — both cover
different, narrower shapes. Confirmed the match directly, not just by
inference: dumped the generated assembly for the original 3-field
`blockdev_read`/`blockdev_write` (via `clang -cc1 -emit-llvm` +
`llvm-link` + `opt -internalize,globaldce` + `llc`, the same pipeline
`eclipse-cc` runs) and found `dev->read_block` (offset 0, first field)
and `dev->ctx` (last field) each compile the way entry #17's own proof
says they should, but `dev->write_block` (the middle field) resolves
through the same `SUB 0,0; DIV`-based runtime halve sequence entry #15
flags as the one that goes wrong for a middle field reached through a
loaded pointer.

**Fix used**: restructure so nothing this code touches has a genuine
middle field — every struct here has exactly two fields, so every field
is either "first" or "last," the two shapes already proven safe:

```c
typedef struct blockdev_ops {
    int (*read_block)(void *ctx, unsigned int blockno, void *buf);
    int (*write_block)(void *ctx, unsigned int blockno, void *buf);
} blockdev_ops_t;

typedef struct blockdev {
    const blockdev_ops_t *ops;
    void *ctx;
} blockdev_t;

int blockdev_read(blockdev_t *dev, unsigned int blockno, void *buf) {
    return dev->ops->read_block(dev->ctx, blockno, buf);
}
int blockdev_write(blockdev_t *dev, unsigned int blockno, void *buf) {
    return dev->ops->write_block(dev->ctx, blockno, buf);
}
```

Re-verified after restructuring: `examples/blockdev_test.c` (which
calls `blockdev_read`/`blockdev_write` only — never `dsk_read_block`/
`dsk_write_block` directly) now runs to a clean `HALT` with fully
correct output (transcript below).

This is exactly the kind of finding the task behind this work asked to
be surfaced honestly: real function pointers through a struct field DO
work on this backend, including a statically-initialized global naming
them — but only reliably up to **two fields per struct level**, a real,
narrower-than-textbook-C constraint, not "function pointers don't work
here" and not "needed a switch-based dispatch instead." A flat
integer-tag `switch` in `blockdev_read`/`blockdev_write` was the
documented fallback if *no* function-pointer shape had worked; it
wasn't needed — nesting two 2-field structs was enough, and keeps the
call-site idiom (`blockdev_read(&dev, blockno, buf)`) exactly what a
kernel programmer coming from any other C target would expect.

Scope, deliberately narrow: one 256-word sector per call (every real
backend's own natural transfer unit), polled completion only, `buf`
restricted to a plain non-extended pointer (0-32767) — matching every
existing driver in this project (DSK, Vulcan, Kismet, Zebra all share
this restriction already).

## The DSK backend (`examples/dsk.h`, `examples/dsk.c`)

`dsk_read_block(blockno, buf)` / `dsk_write_block(blockno, buf)` are
`examples/disk_probe.s`'s already-verified register sequence
(`STORAGE_NOTES.md`), converted to reusable C functions instead of a
one-off hand-assembled probe:

- Bare, unpulsed `DOA`/`DOB` for `dsk_da`/`dsk_ma`, exactly as
  `disk_probe.s` and `STORAGE_NOTES.md` require — `eclipse_io.h`'s
  `outa`/`outb` macros always emit the combined S-pulsed form
  (`DOAS`/`DOBS`), which would wrongly trigger a transfer on DSK the
  instant either register-set happened (DSK triggers on **any** S/P
  pulse on **any** register-setting instruction, not just the one that
  logically "means" start — confirmed directly in `nova_dsk.c`, see
  `STORAGE_NOTES.md`). A separate bare `NIOS`/`NIOP` starts the
  transfer, matching `disk_probe.s` and `vulcan.c`'s own `dskp_doa`/
  `dskp_doc` pattern for the same reason.
- `dsk_read_block`/`dsk_write_block` each poll `SKPDN 020` to
  completion, then read status via bare `DIA` (0 = no error, matching
  `disk_probe.s`).
- `dsk_blockdev_read`/`dsk_blockdev_write` are the two-line adapters
  conforming this shape to `blockdev.h`'s `(ctx, blockno, buf)` dispatch
  shape (`ctx` ignored — DSK is a single-unit device); `dsk_ops` is the
  statically-initialized `blockdev_ops_t` naming them.

### A second, unrelated backend quirk found and worked around: `%=` label substitution requires a real operand

While writing `dsk_read_block`'s polling loop as a *separate*,
operand-less `asm volatile("wait%=:\n\t...")` block (following the
label-uniqueness convention `eclipse_rt.c`'s `putchar`/`getchar` already
use), `dgasm` rejected the generated assembly outright:
`Unexpected character: '%' at line 236`. The generated `.s` showed the
literal text `dsk_rwait%=:` — never substituted with a unique number.

Confirmed as a real, reproducible backend limitation with a minimal
isolated repro (not assumed from one failure):

```c
void f(void) {
    asm volatile("repro_wait%=:\n\tJMP repro_wait%=\n\t");           // no operand
}
void g(int c) {
    asm volatile("repro_wait%=:\n\tJMP repro_wait%=\n\t" :: "r"(c)); // has an operand
}
```

`llc`'s own output: `f`'s label stays literal `repro_wait%=:` /
`JMP repro_wait%=`; `g`'s becomes `repro_wait0:` / `JMP repro_wait0`,
correctly substituted. **This backend's inline-asm `%=` substitution
only fires when the asm block has at least one real operand** —
`eclipse_rt.c`'s own `putraw`/`getchar` never hit this because their
wait loops are folded into the *same* asm block as the operand-bearing
`DOAS`/`DIAC` instruction, never a separate operand-less block.

Not fixed at the backend level (out of scope for this task — this is a
driver-writing task, not a backend-codegen task) and not a design
change: worked around by giving `dsk_read_block`'s/`dsk_write_block`'s
wait loops plain, fixed label names (`dsk_read_wait`/`dsk_write_wait`)
instead of `%=`-suffixed ones. Safe here specifically because both are
ordinary (non-`static inline`, never force-inlined) functions compiled
once — each label is therefore emitted exactly once in the whole
program regardless of how many call sites invoke the function. Anyone
writing a new operand-less inline-asm block with an internal label on
this backend should use the same fixed-label convention, or fold the
label into an operand-bearing block the way `eclipse_rt.c` already does,
rather than assuming `%=` alone is enough.

## Real test evidence: `examples/blockdev_test.c`

Compiled through the full pipeline (three source files, DSK backend +
generic dispatch layer + test program):

```
$ PATH="$HOME/dev/dgasm-src:$PATH" ./eclipse-toolchain/eclipse-cc \
    -o /tmp/blockdev_test.simh \
    examples/blockdev.c examples/dsk.c examples/blockdev_test.c
eclipse-cc: wrote /tmp/blockdev_test.simh
```

Run for real (block 7, three marker words — first, second, and *last*
word of the 256-word sector, the same three `disk_probe.s` already
used, so this is directly comparable to that established baseline):

```
$ rm -f blockdev_test.img
$ { echo 'attach dsk blockdev_test.img'; cat /tmp/blockdev_test.simh; \
    echo 'dep PC 50'; echo 'run 50'; echo 'quit'; } | ~/dev/simh-src/BIN/eclipse

%SIM-INFO: DSK: creating new file
%SIM-INFO: DSK: buffering file in memory
wstatus=0
rstatus=0
rbuf0=123456
rbuf1=377
rbuf255=177777
wbuf0=123456
wbuf1=377
wbuf255=177777

HALT instruction, PC: 00057 (LDA 2,23)
%SIM-INFO: DSK: writing buffer to file: ./blockdev_test.img
```

`wstatus`/`rstatus` both `0` (no device error on either transfer, going
through `blockdev_write`/`blockdev_read` — not `dsk_write_block`/
`dsk_read_block` directly). All three marker words came back in `rbuf`
(a completely separate buffer from `wbuf`, read via a *different*
function-pointer call than the one that wrote them) identical to what
was written, and `wbuf` itself is confirmed unchanged after the round
trip.

**Independent second check**, same methodology `STORAGE_NOTES.md`
used — read the raw backing file directly at block 7's byte offset
(`7 × 256 words × 2 bytes = 3584`), outside the simulator entirely:

```
$ python3 -c "
import struct
with open('blockdev_test.img','rb') as f:
    data = f.read()
off = 7*256*2
w = struct.unpack_from('<256H', data, off)
print(oct(w[0]), oct(w[1]), oct(w[255]))
"
0o123456 0o377 0o177777
```

Matches exactly. This confirms the whole call chain — `main()`'s
`blockdev_write(&dsk_dev, ...)` → `blockdev_write()`'s
`dev->ops->write_block(...)` indirect call → `dsk_blockdev_write()` →
`dsk_write_block()` → real `DOA`/`DOB`/`NIOP`/`SKPDN` — reaches real
hardware-modeled storage, not just in-process state.

(For contrast: the very first, flat-3-field-struct version of this same
test — everything else identical — hung/trapped, `Step expired, PC:
00000 (JMP 0)`, under a 200,000-step budget. That's the failure the
interface-design section above traces to its root cause, not a
separate, unexplained flake.)

## The console driver (`examples/console.h`, `examples/console.c`)

`console_putchar(c)` / `console_getchar()` — raw, single-character
TTI/TTO I/O, no line discipline. Device codes and the exact Busy/Done
polling idiom are the ones already established in this project's
interrupt-driven I/O work (`~/dev/interrupt_test1.s`/`interrupt_test12.s`:
`dev TTO = 011`, `DOAS 0,011`, `NIOC 011`) and confirmed directly
against `eclipse-toolchain/rt/eclipse_rt.c`'s own comment (`"Console
device codes ... TTI = 010, TTO = 011"`) and its `putchar`/`getchar`
implementation, read in full before writing this file:

- `console_putchar`: `DOAS %0,011` + poll `SKPDN 011` — the same
  `outa()`-equivalent idiom `eclipse_io.h` already generalizes, written
  out by hand here (matching `vulcan.c`/`dsk.c`'s own style) so
  `console.c` has zero `#include` dependency on `eclipse_io.h` or `rt/`
  at all.
- `console_getchar`: one-time `NIOS 010` enable pulse (TTI doesn't
  respond to anything before this — already confirmed empirically by
  `eclipse_rt.c`'s own `getchar`), then poll `SKPDN 010` + `DIAC 010`
  (the clear-pulse read — a plain `DIA` would leave `Done` set and a
  second call would silently return the same stale character, per
  `eclipse_rt.c`'s own comment on this exact point).

**Deliberate, checked-for overlap with `eclipse-toolchain/rt/`**: `rt/`
already has `putchar`/`getchar` (`eclipse_rt.c`), always linked in as
part of the user-level C runtime alongside `printf`/`scanf`/the
soft-float runtime/etc. `console.c` is not a duplicate by oversight —
it's a **separate, standalone, kernel-level primitive** with none of
that baggage: no `stdio.h`/`eclipse_rt.h` dependency, so kernel code can
link it alone. It's also *more* minimal than `rt/`'s version on purpose:
`rt/`'s `putchar` does a CR/LF line-discipline translation (`'\n'` ->
`'\r'` + `'\n'`, needed on real hardware terminals, not SIMH's pty) that
a raw driver shouldn't bake in — a kernel wants the bare primitive so it
(or a tty layer built on top of it later) can decide that policy, the
same separation a real OS keeps between a UART driver and the line
discipline above it. `console_test.c` (below) never includes `<stdio.h>`
or links `eclipse_rt.c`'s reachable code at all, confirming `console.c`
really does stand alone.

## Real test evidence: `examples/console_test.c`

No `<stdio.h>` — this program only ever calls `console_putchar`/
`console_getchar`. Writes a known marker string, then echoes back 5
characters read via `console_getchar`.

```
$ ./eclipse-toolchain/eclipse-cc -o /tmp/console_test.simh \
    examples/console.c examples/console_test.c
eclipse-cc: wrote /tmp/console_test.simh
```

Input for the echo phase was injected into the simulated TTI device via
SIMH's own `SEND` command (`"provides a way to insert input into the
console device of a simulated system as if it was entered by a user"`,
per `help send`) — not typed interactively, but a real mechanism the
simulator itself provides for exactly this:

```
$ { cat /tmp/console_test.simh; \
    echo 'dep PC 50'; echo 'send "ABCDE"'; echo 'run 50'; echo 'quit'; } \
    | ~/dev/simh-src/BIN/eclipse

CONSOLE-OUT-OK
ABCDE
ECHO-DONE

HALT instruction, PC: 00057 (LDA 1,161)
```

`CONSOLE-OUT-OK` is `console_putchar`'s write path, unassisted.
`ABCDE` is the 5 characters `SEND` injected into TTI, echoed back out
character-by-character through `console_getchar()` immediately followed
by `console_putchar()` — proving both directions of the driver work,
not just output. `ECHO-DONE` confirms the program ran to completion
(not a hang) after all 5 reads. This is a real captured SIMH transcript,
same as every other test in this file — not a description of expected
behavior.

## Plugging in a future backend (Zebra / Vulcan / Kismet / Argus)

Documented as an extension point directly in `examples/blockdev.h`
(not implemented here — those drivers are being scoped/unified
elsewhere in parallel, and none of their files were touched by this
work). Summary: each already exists as its own `foo_read_block(unit,
blockno, buf)` / `foo_write_block(unit, blockno, buf)` pair (`vulcan.h`,
`kismet.h`, `zebra.h`) — plugging one in needs two small adapter
functions unwrapping `ctx` back into a `unit` number, one shared
`static const blockdev_ops_t`, and one `blockdev_t` per physical unit
(`ctx` packing the unit number into a `void*`, the same "just a value"
convention this backend already uses for function pointers elsewhere —
see `DEBUGGING_NOTES.md` entry #17). No change to `blockdev.h`,
`blockdev.c`, `dsk.c`, or `blockdev_test.c` would be needed. The
two-fields-per-struct-level constraint this task found applies to any
future backend's own adapters/ops-table the same way it applies to
DSK's — worth keeping in mind if a future backend's own driver ever
wants a fatter dispatch struct of its own.

## Regression check

Only new files added (`examples/blockdev.h`, `examples/blockdev.c`,
`examples/dsk.h`, `examples/dsk.c`, `examples/blockdev_test.c`,
`examples/console.h`, `examples/console.c`, `examples/console_test.c`,
this document) — no existing file was modified, so existing baselines
are unaffected by construction. (Several *other* files in this tree
were modified/added by unrelated, parallel work on the Zebra/Vulcan/
Kismet drivers during this session — `examples/vulcan.*`,
`examples/kismet.*`, `examples/zebra.*`, `examples/dskp_common.*` — none
of that is this task's own change and none of it was touched here.)
