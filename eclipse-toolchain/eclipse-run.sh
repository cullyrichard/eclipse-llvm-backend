#!/usr/bin/env bash
# eclipse-run.sh: compile a C source file and load it straight onto real
# Eclipse hardware over a serial line, then drop into an interactive
# terminal session.
#
# Merges two previously-separate scripts:
#   - eclipse-compile.sh: C -> DG absolute-binary (.ab) via the LLVM
#     backend + dgasm -f ab (see that script for the full compile
#     pipeline breakdown).
#   - basrunXB.sh: the serial bootstrap sequence that loads the tiny
#     Absolute Program Loader (APL) from tape, then feeds it a .ab file
#     to load and run.
#
# What changed from basrunXB.sh, and why:
#   - The "cat SOS_XBASIC.AB" step now cats the freshly-compiled .ab
#     file instead of a fixed BASIC image.
#   - The start command is "100R", not "377R". 377 was BASIC's own
#     entry point; every program this toolchain produces starts
#     execution at octal address 100 (see the "org 0100" / "_start:"
#     header on any file eclipse-compile.sh or eclipse-cc emits) --
#     confirm this still holds if either script's output format ever
#     changes.
#   - The date/time answers ("01-01-80", "10:10") are gone. Those were
#     Extended BASIC's own startup prompts, not something a generic
#     compiled C program has any reason to expect. If *your* program
#     happens to read from stdin/console on startup, you'll need to
#     feed it manually once the interactive terminal opens at the end,
#     or add equivalent `echo ... > "$PORT"` lines back in below.
#   - Everything about the APL bootstrap itself (stty setup, "10L", the
#     APL tape image, "77777R") is untouched -- that part loads a
#     fixed, program-independent bootstrap loader and has nothing to
#     do with what gets loaded after it.
#
# This assumes it's being run on whatever machine has the Eclipse's
# serial line wired up as $PORT (basrunXB.sh's own paths implied a
# different machine/home directory than this LLVM toolchain checkout --
# adjust APL_FILE, PORT, and TOOLCHAIN's location below for wherever
# you're actually running this from). Not verified against real
# hardware by me -- eclipse-compile.sh's own header notes -f ab has
# only been checked for well-formedness, not tried on the actual
# target, and I have no way to test the serial/bootstrap half at all.
# Try it on something disposable first if you can.
#
# Usage: eclipse-run.sh input.c

set -euo pipefail

if [ $# -lt 1 ]; then
  echo "usage: eclipse-run.sh input.c" >&2
  exit 1
fi

src="$1"

# --- adjust these for your environment ---
PORT="${PORT:-/dev/ttyUSB0}"
APL_FILE="${APL_FILE:-$HOME/apl.apl}"
COMPILE_SCRIPT="${COMPILE_SCRIPT:-$HOME/dev/eclipse-compile.sh}"
START_ADDR="${START_ADDR:-100}"
PORT_WAIT_SECS="${PORT_WAIT_SECS:-30}"
# ------------------------------------------

if [ ! -x "$COMPILE_SCRIPT" ]; then
  echo "eclipse-run.sh: not found or not executable: $COMPILE_SCRIPT" >&2
  echo "  Set COMPILE_SCRIPT if eclipse-compile.sh lives elsewhere." >&2
  exit 1
fi
if [ ! -f "$APL_FILE" ]; then
  echo "eclipse-run.sh: not found: $APL_FILE" >&2
  echo "  Set APL_FILE to the Absolute Program Loader tape image." >&2
  exit 1
fi

# Under WSL, $PORT only exists once the USB-serial adapter has been
# `usbipd bind`'d and `usbipd attach`'d from Windows -- both steps have
# to be redone after every unplug/replug or Windows reboot (bind
# persists, attach doesn't). Rather than fail deep inside stty with a
# confusing "No such file or directory", poll for it up front and print
# the exact fix if it never shows up.
wait_for_port() {
  if [ -c "$PORT" ]; then
    return 0
  fi
  echo "=== Waiting for $PORT (up to ${PORT_WAIT_SECS}s) ==="
  local waited=0
  while [ ! -c "$PORT" ]; do
    if [ "$waited" -ge "$PORT_WAIT_SECS" ]; then
      echo "eclipse-run.sh: $PORT never showed up after ${PORT_WAIT_SECS}s." >&2
      echo "  If you're on WSL, the USB-serial adapter needs to be attached" >&2
      echo "  from Windows first -- in an Administrator PowerShell:" >&2
      echo "    usbipd list                          # find the adapter's BUSID" >&2
      echo "    usbipd bind --busid <BUSID>           # one-time per device" >&2
      echo "    usbipd attach --wsl --busid <BUSID>   # after every replug/reboot" >&2
      echo "  Set PORT_WAIT_SECS to wait longer, or PORT if it's not $PORT." >&2
      exit 1
    fi
    sleep 1
    waited=$((waited + 1))
  done
  echo "$PORT is up."
  echo
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
ab="$work/$(basename "${src%.*}").ab"

echo "=== Compiling $src -> $ab ==="
"$COMPILE_SCRIPT" "$src" "$ab"

echo
wait_for_port

echo "=== Loading onto Eclipse via $PORT ==="
stty -F "$PORT" 4800 cs8 -parenb raw
stty < "$PORT"
echo

echo "Start loading from paper tape."
echo -n "10L" > "$PORT"
sleep 1
echo

echo "Send the Absolute Loader"
cat "$APL_FILE" > "$PORT"
sleep 1
echo

echo "Start the Absolute Loader"
echo -n "77777R" > "$PORT"
sleep 1
echo

echo "Send the compiled program ($ab)"
cat "$ab" > "$PORT"
sleep 1
echo


echo "Start interactive terminal"
screen "$PORT" 4800,cs7
