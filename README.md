# Eclipse LLVM Backend — Setup Package
# THIS PACKAGE WAS WRITTEN WITH CLAUDE. IT HAS BEEN TESTED WITH REAL HARDWARE. THERE ARE ALMOST CERTAINLY BUGS AND EDGE CASES. PROCEED WITH CAUTION.

This is a real LLVM/Clang backend targeting the Data General Nova/Eclipse
S/140 (a 16-bit minicomputer from 1979-1986), plus a small toolchain wrapper
and runtime library for compiling and running actual C programs on it (via
`eclipseemu`, a real Eclipse simulator, or real hardware).

This package gets you from a clean machine to a working build. Everything
here has been built and verified end-to-end — compiled, assembled, and
run on `eclipseemu` (and, for `examples/test_fps_add.c`, on real Eclipse
hardware) — before being packaged. There are no known open bugs as of
this package; see `DEBUGGING_NOTES.md` and `SOFT_FLOAT_NOTES.md` for the
full history of what was found and fixed along the way, which is worth
reading before touching this backend further — several of the bugs
described there are the kind that are easy to reintroduce by accident.

## What's in this package

- `eclipse-backend.patch` — a git patch containing the entire Eclipse
  backend (LLVM target + Clang frontend support), to apply on top of a
  specific upstream `llvm-project` commit.
- `eclipse-toolchain/` — the toolchain:
  - `eclipse-cc` — compiler driver targeting `eclipseemu` (the
    simulator). Start here.
  - `eclipse-compile.sh` / `eclipse-run.sh` — compiler driver targeting
    **real Eclipse hardware** (DG object/loader `-f ab` format instead
    of `-f simh`); `eclipse-run.sh` additionally handles the serial
    bootstrap-loader sequence and drops you into an interactive terminal
    on the real machine. Needs a serial connection to actual hardware
    (see the comments at the top of each script for the USB/WSL
    passthrough setup, if applicable).
  - `reorder_asm.py` — a required post-processing pass (dgasm has no
    linker/sections, so page-zero data must precede code textually; see
    its own header comment for the full reasoning, including the
    long-jump relaxation it also does).
  - `rt/` — the C runtime library: `stdio.h` (`printf`/`scanf`, `%o`
    support, plus `print_float(float)` for printing floats — deliberately
    a separate function rather than `printf`'s `%f`, see
    `SOFT_FLOAT_NOTES.md` for why), `string.h`, `stdlib.h`, `ctype.h`,
    device I/O macros, and (`rt/eclipse_rt.c`'s tail half) a full
    IEEE-754 single-precision soft-float implementation — see
    `SOFT_FLOAT_NOTES.md`.
- `examples/` — known-working test programs. `examples/fps.h` factors
  out the FPS100 (device `054`) driver primitives (`fpu_out`/`fpu_in`,
  the `cmd_`/`fn_` protocol constants) from `test_fps_add.c` so any
  future FPS100 program can reuse them. `examples/test_float*.c` are the
  soft-float verification programs (see `SOFT_FLOAT_NOTES.md` for
  expected output and the page-zero-budget caveat on combining many of
  them into one program).
- `DEBUGGING_NOTES.md` — the `test_fps_add.c` investigation: what was
  tried, what was ruled out, and the actual root cause (now resolved).
- `SOFT_FLOAT_NOTES.md` — the soft-float implementation: what's
  supported, and the genuine backend bugs (not float-specific — general
  32-bit-arithmetic and calling-convention defects) found and fixed
  while building it.

## Prerequisites

- Linux or WSL2/Ubuntu (this was built and tested under WSL2/Ubuntu).
- Standard build tools: `cmake`, `ninja` or `make`, a C++ compiler,
  `git`, `python3`.
- **Disk space**: budget ~20-30 GB for a full `llvm-project` checkout plus
  build directory.
- **Time**: the initial LLVM+Clang build takes a while — expect
  30-90+ minutes depending on CPU core count and disk speed. Incremental
  rebuilds after touching just the Eclipse backend files are much faster
  (a minute or two), since it's a shared-library build.

## 1. Clone llvm-project at the right commit

The patch applies cleanly on top of this exact upstream commit:

```bash
git clone https://github.com/llvm/llvm-project.git
cd llvm-project
git checkout 8307b46d3ad5ace00c21e1fec6ef4ef4284290e9
```

(A different commit may also work if the patch applies cleanly — this is
just the commit everything here was actually built and tested against.)

## 2. Apply the Eclipse backend patch

From inside `llvm-project`:

```bash
git apply /path/to/eclipse-package/eclipse-backend.patch
```

This adds `llvm/lib/Target/Eclipse/` (the actual backend: register info,
instruction selection, frame lowering, assembly printer, etc.) and Clang
frontend support (`clang/lib/Basic/Targets/Eclipse.{h,cpp}`,
`clang/lib/CodeGen/Targets/Eclipse.cpp`, `clang/lib/Sema/SemaEclipse.{h,cpp}`
for `__attribute__((interrupt))`), plus small touch-ups to the shared files
that register a new target/attribute (`Triple.h`, `Attr.td`, `Sema.h`, a
few `CMakeLists.txt`s, etc.).

If `git apply` complains about a conflict, check `git log` on the commit
above vs. what you actually checked out — the patch is sensitive to exact
line numbers in a handful of shared files.

## 3. Configure and build

```bash
mkdir llvm-build && cd llvm-build
cmake -G "Unix Makefiles" ../llvm-project/llvm \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_TARGETS_TO_BUILD=X86 \
  -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=Eclipse \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DLLVM_USE_LINKER=gold

make clang -j$(nproc)
```

(`Ninja` works too if you prefer — swap the generator and use `ninja
clang`. `BUILD_SHARED_LIBS=ON` is what makes incremental rebuilds of just
the Eclipse backend fast — a full static build works too, but every
rebuild will relink the whole `clang` binary.)

`llvm-build/bin/clang` and `llvm-build/bin/llc` are what you'll use from
here on. Sanity-check the target is registered:

```bash
llvm-build/bin/clang -cc1 -triple eclipse-dg-none -S -o - /dev/null
```

This should print a small `.s` preamble (`org 0100`, `dev TTI = 010`,
etc.) with no errors.

## 4. Install `dgasm` (the assembler)

`dgasm` is a real Nova/Eclipse assembler, separate project:

```bash
git clone https://github.com/CWood1/dgasm
cd dgasm
cmake .
make
sudo make install    # or just add ./dgasm to your PATH
```

Verify: `dgasm -t eclipse_s140 -f simh -o /dev/null /dev/null` should run
without a "command not found."

## 5. Install `eclipseemu` (the simulator)

This is just the standard SIMH package's Eclipse simulator — no custom
build needed:

```bash
sudo apt-get install simh
```

This installs a binary literally named `eclipseemu` (confirmed via
`dpkg -S /usr/bin/eclipseemu` → package `simh`). If your distro's package
doesn't provide that exact binary name, look for whatever SIMH names its
Eclipse simulator (sometimes just `eclipse`) and adjust accordingly.

## 6. Point the toolchain at your build

`eclipse-cc` (and `eclipse-compile.sh`) default to `$HOME/dev/llvm-build`
for the `clang`/`llc` binaries. If yours lives elsewhere, override it via
the `LLVM_BUILD` environment variable rather than editing the script:

```bash
LLVM_BUILD=/path/to/your/llvm-build ./eclipse-toolchain/eclipse-cc ...
```

`eclipse-compile.sh` also respects `TOOLCHAIN` (defaults to
`$HOME/dev/eclipse-toolchain`) the same way, if you're not running it
from inside this package's own directory layout.

## 7. Smoke test

```bash
cd eclipse-package
./eclipse-toolchain/eclipse-cc -o /tmp/test.simh examples/sizeof_check.c
```

Then run it:

```bash
{ cat /tmp/test.simh; echo 'dep PC 100'; echo 'step 5000'; echo 'e PC'; echo 'quit'; } | eclipseemu
```

You should see a series of numbers printed (1, 2, 2, 4, 2, 4, 10, 14 —
various `sizeof()` results) followed by `HALT instruction`. If you see
that, the whole toolchain is working end to end.

`examples/printf_octal_check.c` and `examples/isr_c_test.c` are two more
known-good smoke tests — the interrupt one (`isr_c_test.c`) needs its
generated `.s` file's interrupt vector manually wired if you're calling
`clang`/`llc` directly instead of through `eclipse-cc`; see the comment at
the top of that file, or just use `eclipse-cc` which handles it
automatically.

## 8. Float smoke test

```bash
./eclipse-toolchain/eclipse-cc -o /tmp/test_float_mul.simh examples/test_float_mul.c
{ cat /tmp/test_float_mul.simh; echo 'dep PC 100'; echo 'run 100'; echo 'quit'; } | eclipseemu
```

You should see one `eclipse-cc: retrying with ... protected` message on
stderr (expected — `llc` only discovers each needed soft-float symbol
after `opt`'s dead-code pass has already run, so a retry is normal; see
`SOFT_FLOAT_NOTES.md`), followed by `10`, `-6`, `100`. `test_float_div.c`
(`2`, `3`, `-3`), `test_float_cmp.c` (`1 0 1 1 1 0`), and
`test_float_conv.c` (`42`, `100`, `-17`) exercise the other capabilities
individually — each fits comfortably within the page-zero budget.
`test_float.c` combines *all* of them into one program and is expected
to exceed that budget — see its own header comment and
`SOFT_FLOAT_NOTES.md`'s "Known limit" section.

`examples/test_print_float.c` exercises `print_float` — the way to
actually *print* a float (see "Why print_float isn't wired into printf"
in `SOFT_FLOAT_NOTES.md` for why it isn't `printf("%f", ...)`):

```bash
./eclipse-toolchain/eclipse-cc -o /tmp/test_print_float.simh examples/test_print_float.c
{ cat /tmp/test_print_float.simh; echo 'dep PC 100'; echo 'run 100'; echo 'quit'; } | eclipseemu
```

Expect `3.000000`, `0.500000`, `-2.250000`, `100000.000000` — no retry
message this time (`print_float` needs no float-arithmetic libcalls, by
design; see `SOFT_FLOAT_NOTES.md`).

## Real hardware

`eclipse-compile.sh input.c output.ab` compiles to the real DG
object/loader format instead of the `eclipseemu`-only `-f simh` format.
`eclipse-run.sh input.c` goes further: compiles, then drives the actual
serial bootstrap-loader sequence to load and start the program on real
hardware, then drops you into an interactive terminal on the machine.
Both need a serial connection to actual Eclipse hardware — see the
comments at the top of each script (environment variables like `PORT`,
`APL_FILE`, `START_ADDR` are all overridable the same way `LLVM_BUILD`
is above).

## Where to go from here

Both known investigations in this package (`test_fps_add.c`'s
real-hardware bug, and the soft-float implementation) are resolved as of
this package — see `DEBUGGING_NOTES.md` and `SOFT_FLOAT_NOTES.md`
respectively for the full history, including several genuine backend
bugs that are worth understanding before extending this backend further
(the same class of bug — e.g. the DAG-combine and multi-word-global
issues in `SOFT_FLOAT_NOTES.md` — could easily resurface in new code that
touches wide (32-bit) values or multi-word globals).

Natural next steps, roughly in order of how contained they are:
- Genuine 64-bit `double` (f64) soft-float support — `double` currently
  aliases 32-bit `float` (see `SOFT_FLOAT_NOTES.md`), which is fine for
  most purposes but loses precision on values that need real double
  range/precision. Real support needs the same approach as `float`, but
  needs genuine 64-bit integer support this backend doesn't have yet
  (see the main backend `README.md`'s "Known limitations").
- Extending frame-relative addressing beyond the current ±127-word limit
  (mirroring the existing page-zero indirect-addressing mechanism) —
  would reduce how much the soft-float functions (and any other
  sufficiently complex function) need to be manually split to fit.
- Real hardware FPU opcodes as an alternative/complement to soft-float
  (see the main backend `README.md`'s "No hand-verified Eclipse FPU
  opcodes" limitation) — deliberately not attempted without the same
  `eclipseemu`-verification rigor everything else here used.
