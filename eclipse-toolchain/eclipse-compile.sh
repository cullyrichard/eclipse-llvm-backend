#!/usr/bin/env bash
# eclipse-compile.sh: compile a C source file to DGASM's real Eclipse
# object/loader format, for physical Eclipse hardware (not the SimH
# simulator), step by step, matching exactly:
#   1. clang -cc1            -- C source -> LLVM IR
#   2. clang -cc1            -- runtime library -> LLVM IR
#   3. llvm-link              -- merge into one module
#   4. opt -internalize,globaldce -- strip runtime-library functions the
#                                     program doesn't actually call (page-zero
#                                     is a hard, shared 256-word budget)
#   5. llc                    -- LLVM IR -> Eclipse assembly
#   6. reorder_asm.py         -- reorder page-zero data before code,
#                                 relax out-of-range jumps
#   7. dgasm -t eclipse_s140 -f eclipse -- real assembler, DG Eclipse
#                                          object/loader format
#
# NB: -f eclipse output has only been checked for well-formedness against
# dgasm itself (see the module's own docs) -- it has not been verified
# against real Eclipse hardware or a real paper-tape/console loader in
# this project, unlike the -f simh path this script replaces (which was
# run and checked against eclipseemu repeatedly). Treat it accordingly
# until it's been tried on the actual target.
#
# Usage: eclipse-compile.sh input.c [output.eclipse]

set -euo pipefail

if [ $# -lt 1 ]; then
  echo "usage: eclipse-compile.sh input.c [output.eclipse]" >&2
  exit 1
fi

src="$1"
out="${2:-${src%.*}.eclipse}"

LLVM_BUILD="${LLVM_BUILD:-$HOME/dev/llvm-build}"
TOOLCHAIN="${TOOLCHAIN:-$HOME/dev/eclipse-toolchain}"
CLANG="$LLVM_BUILD/bin/clang"
LLC="$LLVM_BUILD/bin/llc"
LLVM_LINK="$LLVM_BUILD/bin/llvm-link"
OPT="$LLVM_BUILD/bin/opt"
REORDER="$TOOLCHAIN/reorder_asm.py"
RT_SRC="$TOOLCHAIN/rt/eclipse_rt.c"
TRIPLE="eclipse-dg-none"

for tool in "$CLANG" "$LLC" "$LLVM_LINK" "$OPT"; do
  if [ ! -x "$tool" ]; then
    echo "eclipse-compile.sh: not found or not executable: $tool" >&2
    echo "  LLVM_BUILD is currently: $LLVM_BUILD" >&2
    echo "  Set it explicitly if your llvm-build directory lives elsewhere, e.g.:" >&2
    echo "    LLVM_BUILD=/path/to/llvm-build $0 $*" >&2
    exit 1
  fi
done
if [ ! -d "$TOOLCHAIN" ]; then
  echo "eclipse-compile.sh: not found: $TOOLCHAIN" >&2
  echo "  TOOLCHAIN is currently: $TOOLCHAIN" >&2
  echo "  Set it explicitly if eclipse-toolchain lives elsewhere, e.g.:" >&2
  echo "    TOOLCHAIN=/path/to/eclipse-toolchain $0 $*" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

RESDIR="$("$CLANG" -print-resource-dir)"
CC1_FLAGS=(-cc1 -triple "$TRIPLE" -nostdsysteminc
           -isystem "$RESDIR/include"
           -I "$TOOLCHAIN/rt/include" -I "$TOOLCHAIN/rt")

echo "[1/7] clang -cc1: $src -> IR"
"$CLANG" "${CC1_FLAGS[@]}" -emit-llvm "$src" -o "$work/prog.ll"

echo "[2/7] clang -cc1: runtime library -> IR"
"$CLANG" "${CC1_FLAGS[@]}" -emit-llvm "$RT_SRC" -o "$work/rt.ll"

echo "[3/7] llvm-link: merge"
"$LLVM_LINK" -S "$work/prog.ll" "$work/rt.ll" -o "$work/merged.ll"

echo "[4-7/7] opt/llc/reorder/dgasm: assemble to DG object/loader format"
# Soft-float symbols need special handling: llc's own codegen inserts
# calls to these (float arithmetic/compare/convert "softened" into
# libcalls during instruction selection) *after* the opt pass below has
# already run, so they look unreferenced to globaldce and get stripped
# even on programs that need them — confirmed empirically ("Undefined
# symbol: __fixsfsi" at the dgasm step) before this was handled.
# Protecting all of them unconditionally, always, costs real page-zero
# budget even for a program using just one float op (see eclipse_rt.c's
# soft-float section and README.md's "Known limitations" for the shared
# 256-word ceiling this can bump into) — so instead, iteratively
# discover exactly which ones this specific program needs: each retry
# adds only the symbols dgasm *just* reported undefined and tries again,
# stopping as soon as a pass reports no *new* undefined-symbol names
# (either success, or a genuine failure that finding more names won't fix).
SOFTFLOAT_SYMS="__addsf3,__subsf3,__mulsf3,__divsf3,__eqsf2,__nesf2,__ltsf2,__lesf2,__gtsf2,__gesf2,__unordsf2,__floatsisf,__floatunsisf,__fixsfsi,__fixunssfsi"

build_and_assemble() {
  local pub_api="$1"
  "$OPT" -S -passes="internalize,globaldce" \
    -internalize-public-api-list="$pub_api" \
    "$work/merged.ll" -o "$work/stripped.ll"
  "$LLC" -mtriple="$TRIPLE" -filetype=asm "$work/stripped.ll" -o "$work/prog.s"
  python3 "$REORDER" "$work/prog.s" "$work/prog_r.s"
  dgasm -t eclipse_s140 -f ab -o "$out" "$work/prog_r.s"
}

protected="main"
pass=1
while :; do
  if output="$(build_and_assemble "$protected" 2>&1)"; then
    echo "$output"
    break
  fi
  missing="$(echo "$output" | grep -oE 'Undefined symbol: __[A-Za-z0-9_]+' \
             | sed 's/Undefined symbol: //' | sort -u)"
  new="$(comm -23 <(echo "$missing") <(echo "$protected" | tr ',' '\n' | sort -u))"
  if [ -z "$new" ] || [ "$pass" -ge 15 ]; then
    echo "$output" >&2
    exit 1
  fi
  echo "eclipse-compile.sh: retrying with $(echo "$new" | tr '\n' ' ')protected (needed by this program)" >&2
  protected="$protected,$(echo "$new" | tr '\n' ',' | sed 's/,$//')"
  pass=$((pass + 1))
done

echo "wrote $out"
