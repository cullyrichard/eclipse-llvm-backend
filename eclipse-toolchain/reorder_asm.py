#!/usr/bin/env python3
"""Reorder llc's Eclipse assembly output so all `var` (page-zero data)
declarations come before all code — required because `LDA`/`STA ac,addr`
default to page-zero (absolute, 8-bit-displacement) addressing, and dgasm
assigns addresses strictly in the order lines appear in the source file
(confirmed by reading dgasm's assembler.c directly: current_address starts
at `org` and increments statement-by-statement). llc's normal per-function
emission order interleaves each function's own constant-pool `var` lines
with its code, and the module-wide jump table at the very end — meaning
growing code can silently push a `var` line past address 255 (the largest
page-zero displacement) and dgasm will reject the file with "Address out
of range".

This is pure line-oriented text manipulation, not a code transformation:
the fixed preamble (comments, `org`, `dev`, `_start`, the initial
`_STACKTOP`/`_STACKLIM`/`_scratch` declarations) stays exactly first, then
every remaining `var `
line moves before every remaining non-`var` line, each group keeping its
own original relative order. See llvm/lib/Target/Eclipse/EclipseAsmPrinter.h
in the Eclipse LLVM backend for why this lives here instead of in the
backend itself.

Full pipeline (see main(), and each stage's own section below):
sanitize_identifiers -> reorder -> dedup_constants -> relax_long_jumps ->
fix_stack_pointer.
dedup_constants merges byte-identical constant-pool entries that LLVM's
per-function MachineConstantPool emits independently in every function
that happens to use the same literal value — confirmed to reclaim 100+
duplicate page-zero words in real programs (see its own section below).
relax_long_jumps must run after dedup_constants since it computes real
addresses from the final line layout, and removing duplicate lines
changes addresses. fix_stack_pointer must run last of all, for the same
"final addresses only" reason, plus one more: relax_long_jumps can itself
grow the program (new long-jump slots), which shifts where the program's
data actually ends — see fix_stack_pointer's own section below for why
that end address matters.

Usage: reorder_asm.py < in.s > out.s
   or: reorder_asm.py in.s out.s
"""
import re
import sys

VAR_RE = re.compile(r"^\s*var\s")

# LLVM names anonymous constants (string literals, etc.) `.str`, `.str.1`,
# `.str.2`, ... — but dgasm's identifier grammar doesn't accept `.`
# (confirmed against the real dgasm binary: "Parse error ... near '.'").
# Every reference to a given symbol is plain text by the time this script
# runs, in the same file, so a straight text substitution keeps every
# declaration and use consistent without touching the backend's C++ symbol
# machinery. Only touches dot-led identifier tokens outside double-quoted
# string data (a string literal's own *contents*, e.g. `var .str = "a.b"`,
# must not be mangled).
DOTTED_IDENT_RE = re.compile(r"\.[A-Za-z0-9_.]+")
QUOTED_RE = re.compile(r'"(?:[^"\\]|\\.)*"')


def sanitize_identifiers(text: str) -> str:
    def fix_line(line: str) -> str:
        parts = []
        pos = 0
        for m in QUOTED_RE.finditer(line):
            parts.append(DOTTED_IDENT_RE.sub(lambda d: d.group(0).replace(".", "_"),
                                              line[pos:m.start()]))
            parts.append(m.group(0))
            pos = m.end()
        parts.append(DOTTED_IDENT_RE.sub(lambda d: d.group(0).replace(".", "_"),
                                          line[pos:]))
        return "".join(parts)

    return "\n".join(fix_line(l) for l in text.splitlines()) + "\n"


# Page-zero-required var lines: pointer slots (CallSlots' "*_SLOT",
# AddrSlots'/LEAGA's "*_PTR", and reorder's own long-jump relaxation
# slots, which also happen to be named "*_SLOT"), constant-pool entries
# ("CPI<n>_<m>"), and LEAGA offset constants ("*_offN", EclipseAsmPrinter
# .cpp's OffsetSlots — the materialized-constant word a nonzero
# GlobalAddress offset gets ADDed against after loading a "*_PTR" slot,
# since this ISA has no immediate-operand ADD) — these are either
# directly page-zero-addressed themselves (constant pool) or must live
# in page-zero so a direct LDA can reach the word at all (every other
# slot kind). Everything else that's a `var` line is a global's *actual
# content* (EclipseAsmPrinter.cpp's emitGlobalVariable) — now reached
# only indirectly through one of those pointer slots (see
# EclipseInstrInfo.td's "page-zero indirect data addressing" comment),
# so it no longer needs to be in page-zero at all, and moves to the end
# of the file instead of the front.
PZ_VAR_RE = re.compile(r"^\s*var\s+(?:\S+_SLOT|\S+_PTR|\S+_off\d+|CPI\d+_\d+)\s*=")


def reorder(text: str) -> str:
    lines = text.splitlines()

    # The fixed preamble is everything through the blank line that follows
    # the initial "var _scratch = ..." declaration (see
    # EclipseAsmPrinter.cpp's emitStartOfAsmFile — that's always emitted
    # first, unconditionally, before any function is processed).
    split_at = len(lines)
    for i, line in enumerate(lines):
        if line.strip().startswith("var _scratch"):
            # consume through the next blank line (or end of file)
            j = i + 1
            while j < len(lines) and lines[j].strip() != "":
                j += 1
            split_at = j + 1 if j < len(lines) else j
            break

    preamble = lines[:split_at]
    rest = lines[split_at:]

    pz_data_lines = [l for l in rest if VAR_RE.match(l) and PZ_VAR_RE.match(l)]
    bulk_data_lines = [l for l in rest if VAR_RE.match(l) and not PZ_VAR_RE.match(l)]
    code_lines = [l for l in rest if not VAR_RE.match(l)]

    out = preamble + pz_data_lines + code_lines + bulk_data_lines
    return "\n".join(out) + "\n"



# --- long-jump relaxation -------------------------------------------------
#
# dgasm's default JMP addressing is PC-relative with an 8-bit *signed*
# displacement (-128..127 words from the word after the JMP itself) —
# fine for a small function, but a big one (e.g. eclipse_rt.c's printf/
# scanf, dozens of basic blocks) can put a loop's back-edge or a
# conditional branch's target out of that range: real dgasm rejects it
# with "Address out of range".
#
# Making *every* internal jump indirect (through a page-zero pointer
# slot, the same trick CALL already uses for every function) was tried
# first and doesn't scale: one slot per distinct branch target, and a
# library with enough functions has enough of those to overflow the
# 256-word page-zero budget on its own — confirmed empirically. So this
# only converts the *specific* jumps that are actually out of range,
# computing real addresses by replicating dgasm's own pass1 statement
# sizing (confirmed by reading its assembler.c directly, not assumed):
# STMT_LABEL and `dev` lines are zero-width; a string `var` is
# `len(string)+1` words; a real *extended-addressing* instruction
# (EXTENDED_INSN_RE below) is 2 words, everything else (a real base-ISA
# instruction, or a numeric/symbol `var`) is exactly 1 word. Iterates to
# a fixed point, since inserting a new slot's `var` line shifts every
# later address and can turn a previously in-range jump out of range too.

ORG_RE = re.compile(r"^\s*org\s+(\S+)")
LABEL_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):\s*(//.*)?$")
VAR_DECL_RE = re.compile(r"^\s*var\s+(\S+)\s*=\s*(.*)$")
DEV_RE = re.compile(r"^\s*dev\s")
JMP_RE = re.compile(r"^(\s*)JMP\s+([A-Za-z_][A-Za-z0-9_]*)\s*$")
STRING_LIT_RE = re.compile(r'^"((?:[^"\\]|\\.)*)"')

# Real Eclipse S/140 extended-addressing instructions (EclipseInstrInfo.td's
# CALL/LEAGA/ELDAGA/ESTAGA defs emit EJSR/ELEF/ELDA/ESTA; EJMP/EISZ/EDSZ/
# ELDB/ESTB round out the same family per the S/140 Programmer's Reference
# and dgasm's opcode.c, though this backend doesn't emit those yet) are a
# genuine 2-word encoding -- the second word holds the real address --
# unlike every base-ISA instruction this backend otherwise emits, which is
# always exactly 1 word (confirmed against dgasm's opcode.c: these are the
# only entries with ENCODING_EXTENDEDFLOW/ENCODING_EXTENDEDLOAD, size 2;
# everything else this backend prints is size 1). compute_addresses below
# has to know this or it silently under-counts the real address of
# anything after one of these -- confirmed the hard way: before this,
# fix_stack_pointer placed `_STACKTOP` under the program's true end
# address on any program with enough EJSR/ELDA/ESTA/ELEF instructions
# before its trailing bulk data to accumulate a several-word discrepancy,
# so the upward-growing hardware stack immediately collided with and
# corrupted that data (c-testsuite 00150 regressed exactly this way when
# the backend started emitting these instructions -- see
# DEBUGGING_NOTES.md).
EXTENDED_INSN_RE = re.compile(
    r"^\s*(EJSR|EJMP|ELEF|ELDA|ESTA|EISZ|EDSZ|ELDB|ESTB)\b")


def dgasm_number(tok: str) -> int:
    """Parse a dgasm numeric literal: C-style — a leading '0' with more
    digits means octal, otherwise decimal (docs/IO_DEVICES.md's "010"
    octal-literal note in the sibling eccc project describes the same
    convention on dgasm's input side)."""
    tok = tok.strip()
    if tok.startswith("-"):
        return -dgasm_number(tok[1:])
    if len(tok) > 1 and tok[0] == "0":
        return int(tok, 8)
    return int(tok, 10)


def _string_literal_word_count(escaped_body: str) -> int:
    # strlen(the *real* string) + 1 (dgasm's null terminator) — count
    # real characters, not the escaped source text's own length.
    n = 0
    i = 0
    while i < len(escaped_body):
        i += 2 if escaped_body[i] == "\\" and i + 1 < len(escaped_body) else 1
        n += 1
    return n + 1


def compute_addresses(lines):
    """Returns (label_addr: name -> address, line_addr: per-line address
    or None for zero-width/non-statement lines, end_addr: the address one
    past the last word dgasm will actually deposit), replicating dgasm's
    own pass1 sequential address assignment."""
    addr = 0o100  # dgasm's own default before any `org`
    label_addr = {}
    line_addr = [None] * len(lines)
    for i, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith("//") or DEV_RE.match(line):
            continue
        m = ORG_RE.match(line)
        if m:
            addr = dgasm_number(m.group(1))
            continue
        m = VAR_DECL_RE.match(line)
        if m:
            name, rhs = m.group(1), m.group(2).strip()
            label_addr[name] = addr
            line_addr[i] = addr
            sm = STRING_LIT_RE.match(rhs)
            addr += _string_literal_word_count(sm.group(1)) if sm else 1
            continue
        m = LABEL_RE.match(stripped)
        if m:
            label_addr[m.group(1)] = addr
            continue
        # A real instruction: 2 words for extended addressing
        # (EXTENDED_INSN_RE above), 1 word for everything else (no
        # immediate operands on the base ISA at all — see
        # EclipseInstrInfo.td's file header — so nothing else is
        # multi-word).
        line_addr[i] = addr
        addr += 2 if EXTENDED_INSN_RE.match(line) else 1
    return label_addr, line_addr, addr


def _data_block_end(lines) -> int:
    """Index right after the last existing *page-zero* `var` line — new
    slot declarations get inserted there. Must use PZ_VAR_RE, not the
    generic VAR_RE: since reorder() moved bulk (non-page-zero) data to
    the *end* of the file, the last `var` line in the whole file is now
    typically down there — inserting a new relaxation slot next to it
    would place the slot nowhere near page-zero, defeating the point."""
    last = 0
    for i, line in enumerate(lines):
        if PZ_VAR_RE.match(line):
            last = i + 1
    return last


def relax_long_jumps(text: str) -> str:
    lines = text.splitlines()
    declared_slots = {
        m.group(1)
        for line in lines
        if (m := VAR_DECL_RE.match(line)) and m.group(1).endswith("_SLOT")
    }

    for _ in range(50):  # bounded fixed-point iteration; see module doc
        label_addr, line_addr, _end_addr = compute_addresses(lines)
        new_slots = []
        changed = False
        for i, line in enumerate(lines):
            m = JMP_RE.match(line)
            if not m or line_addr[i] is None:
                continue
            indent, target = m.groups()
            if target not in label_addr:
                continue
            # PC-relative displacement is relative to the word *after*
            # the JMP itself (confirmed by the exact -128..127 boundary
            # dgasm's own "Address out of range" errors report).
            disp = label_addr[target] - (line_addr[i] + 1)
            if -128 <= disp <= 127:
                continue
            slot = target + "_SLOT"
            lines[i] = f"{indent}JMP @{slot},0"
            changed = True
            if slot not in declared_slots and slot not in new_slots:
                new_slots.append(slot)

        if new_slots:
            insert_at = _data_block_end(lines)
            for slot in new_slots:
                target = slot[: -len("_SLOT")]
                lines.insert(insert_at, f"var {slot} = {target}")
                insert_at += 1
                declared_slots.add(slot)

        if not changed:
            break

    return "\n".join(lines) + "\n"


# --- constant-pool deduplication -------------------------------------------
#
# LLVM's MachineConstantPool is *per function* — the same literal value
# (e.g. the exponent bias, a hidden-bit mask half, a loop bound) used in
# several different functions gets a separate, independently-named CPI
# entry in each one, even though the value is byte-identical. Confirmed
# empirically on eclipse_rt.c's soft-float section (which reuses a small
# set of constants extremely heavily across sf_add/sf_mul/sf_div/
# __fixsfsi/print_float/...): well over 100 duplicate single-word page-
# zero slots in one real program, out of a 256-word total budget.
#
# Every CPI entry is exactly one 16-bit word — EclipseAsmPrinter::
# emitConstantPool emits one `var CPIn_m = value` line per
# MachineConstantPool entry, and this backend's type legalizer splits any
# wider-than-i16 constant into independent i16 halves *before* it reaches
# the constant pool (confirmed by reading emitConstantPool directly: it
# only ever handles a plain ConstantInt via getSExtValue(), never a
# multi-word value) — so entries are safe to dedupe purely by value, with
# no multi-word grouping/pairing to worry about. And every *reference* is
# a bare `LDA/STA ac,CPIn_m` symbol operand — confirmed by grepping real
# generated assembly for every CPI use, none are offset against a
# neighboring entry (e.g. no `CPIn_m+1`) — so nothing depends on any two
# CPI entries being adjacent or a fixed distance apart, and a plain
# "rename every reference to the surviving symbol" is sufficient.
#
# Other page-zero `var` kinds (*_SLOT, *_PTR, *_offN) are deliberately
# NOT touched here: their symbol names are already derived from their
# target (e.g. `var foo_SLOT = foo`), so LLVM's own symbol table already
# gives them natural, automatic sharing — a given target only ever gets
# one slot declaration no matter how many call/reference sites there
# are. CPI entries are the one kind that's genuinely duplicated today,
# because MachineConstantPool has no equivalent cross-function identity.
#
# Runs after reorder() (so the var lines this cares about are already
# gathered together) and before relax_long_jumps() (which computes exact
# addresses from the *final* line layout — it must see the post-dedup
# file, not count the soon-to-be-removed duplicate lines).

CPI_DECL_RE = re.compile(r"^\s*var\s+(CPI\d+_\d+)\s*=\s*(.+?)\s*$")
IDENT_TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def _rewrite_idents(line: str, rename: dict) -> str:
    """Substitutes identifier tokens per `rename`, skipping the contents of
    double-quoted string literals — same quote-awareness as
    sanitize_identifiers above, and for the same reason (a string
    literal's own text must never be touched, even if it happens to
    contain something that looks like a CPI name)."""
    parts = []
    pos = 0
    for m in QUOTED_RE.finditer(line):
        parts.append(IDENT_TOKEN_RE.sub(lambda t: rename.get(t.group(0), t.group(0)),
                                         line[pos:m.start()]))
        parts.append(m.group(0))
        pos = m.end()
    parts.append(IDENT_TOKEN_RE.sub(lambda t: rename.get(t.group(0), t.group(0)),
                                     line[pos:]))
    return "".join(parts)


def dedup_constants(text: str) -> str:
    lines = text.splitlines()
    canonical_for_value = {}
    rename = {}
    drop = set()

    for i, line in enumerate(lines):
        m = CPI_DECL_RE.match(line)
        if not m:
            continue
        name, value = m.group(1), m.group(2)
        existing = canonical_for_value.get(value)
        if existing is None:
            canonical_for_value[value] = name
        else:
            rename[name] = existing
            drop.add(i)

    if not rename:
        return "\n".join(lines) + "\n"

    out = [_rewrite_idents(line, rename) for i, line in enumerate(lines) if i not in drop]
    return "\n".join(out) + "\n"


# --- stack-pointer placement -----------------------------------------------
#
# Bug found while verifying bug #8's fix (see SOFT_FLOAT_NOTES.md), fixed
# again below in a different form once the calling convention moved onto
# real hardware SAVE/RTN (see DEBUGGING_NOTES.md and EclipseInstrInfo.td's
# SAVE/RET comments): the frame/local-storage stack SAVE/RTN manage lives
# at a fixed hardware location (040 octal), initialized once at program
# start (`_STACKTOP`, `_STACKLIM` — EclipseAsmPrinter.cpp's _start
# emission) rather than a plain `_SP` software variable the way it used to
# be. Argument push/pop (EclipseISelLowering.cpp's emitPushPop/
# emitAdjCallStack) and a couple of scratch-save sites
# (EclipseAsmPrinter.cpp, EclipseInstrInfo.cpp's MUL/DIV expansion) target
# that same 040 location directly now too, so there's only ever one real
# stack, not two drifting independently.
#
# Under real hardware SAVE, the stack grows *upward* from `_STACKTOP`
# toward `_STACKLIM` — the opposite direction from the old software
# stack, which grew downward from a high `_SP` toward the program's own
# data sitting below it. That inversion is exactly why the two constants
# need different treatment: `_STACKLIM` just needs to clear dgasm's own
# 65536-word hard ceiling with room to spare, entirely independent of how
# large any particular program's own data is — EclipseAsmPrinter.cpp's
# default (`0177700`, decimal 65472) is already comfortably generous and
# never needs patching. `_STACKTOP`, on the other hand, still needs
# exactly the same dynamic fix `_SP` used to: reorder() above deliberately
# moves every *bulk* (non-page-zero) global to the very end of the file,
# so dgasm deposits it at the *highest* addresses the program uses, and
# that address grows with the linked program's size (more functions
# pulled in — even ones genuinely dead at runtime: `eclipse-cc`'s
# symbol-protection mechanism has to keep a libcall's own code reachable
# to satisfy dgasm's "Undefined symbol" check even for a program that
# never actually reaches it). A `_STACKTOP` placed too close underneath
# that growing address would leave the *upward*-growing stack too little
# room before it starts colliding with `_STACKLIM`'s own ceiling on a
# large enough program — the same class of problem the original `_SP` bug
# was, just approached from the opposite direction. Confirmed directly (of
# the original, downward-growing version of this bug): examining word
# 0102 (the cell holding the live `_SP` value at the time) mid-run on the
# failing repro showed it reaching decimal 7605 while that same program's
# own data extended to 7755 — the stack had already descended into live
# data before the eventual crash. dgasm itself never catches this class of
# bug either way: unlike the hard page-zero (0-255) ceiling it enforces
# for every `LDA`/`STA` displacement, it has no equivalent check for "does
# the stack collide with static data" — to dgasm, both are just plain
# memory words.
#
# Fixed here, not in the backend: EclipseAsmPrinter emits `_STACKTOP`'s
# value long before the final program layout exists (dedup_constants and
# relax_long_jumps haven't run yet, and it has no way to know what else
# will or won't end up linked in). This pass runs last of all, once
# compute_addresses can see the *actual* final end-of-program address, and
# repoints `_STACKTOP` comfortably above it instead of trusting a fixed
# guess. STACK_MARGIN below leaves far more headroom above the program's
# own data than any call/recursion depth this backend's calling
# convention could plausibly reach, while still failing loudly — instead
# of silently emitting an invalid or wrapped address — on the pathological
# case of a program whose own data nearly fills the address space outright.

STACK_MARGIN = 4096  # words of headroom between the program's own highest
                      # data address and _STACKTOP — see this section's
                      # comment for why this is generous.
STACK_LIMIT = 0o177700  # must match EclipseAsmPrinter.cpp's _STACKLIM
                         # default exactly — the real ceiling the
                         # upward-growing hardware stack checks against,
                         # not just dgasm's raw address-space limit.

SP_DECL_RE = re.compile(r"^(\s*var\s+_STACKTOP\s*=\s*)(\S+)(.*)$")


def fix_stack_pointer(text: str) -> str:
    lines = text.splitlines()
    _label_addr, _line_addr, end_addr = compute_addresses(lines)

    new_sp = end_addr + STACK_MARGIN
    if new_sp >= STACK_LIMIT:
        print(
            f"reorder_asm.py: program data extends to address {end_addr} "
            f"({end_addr:#o}) — placing the stack {STACK_MARGIN} words "
            f"above it would reach or exceed _STACKLIM ({STACK_LIMIT:#o}), "
            "the hardware stack's own ceiling. This program is too large "
            "for the stack to fit safely; shrink it (fewer linked runtime "
            "functions, less string data) rather than trusting a smaller "
            "margin.",
            file=sys.stderr,
        )
        sys.exit(1)

    for i, line in enumerate(lines):
        m = SP_DECL_RE.match(line)
        if m:
            lines[i] = f"{m.group(1)}{new_sp}{m.group(3)}"
            break
    else:
        # EclipseAsmPrinter::emitStartOfAsmFile always emits exactly one
        # `var _STACKTOP = ...` line — if that ever stops being true (a
        # backend change, an input file from somewhere else), silently
        # leaving the old value in place would resurrect exactly the bug
        # this pass exists to fix, just without any error to point at.
        # Fail loudly instead.
        raise SystemExit("reorder_asm.py: no `var _STACKTOP = ...` line "
                          "found — expected EclipseAsmPrinter to always "
                          "emit one")

    return "\n".join(lines) + "\n"


def main() -> int:
    if len(sys.argv) == 3:
        with open(sys.argv[1], "r") as f:
            text = f.read()
        result = fix_stack_pointer(relax_long_jumps(dedup_constants(reorder(sanitize_identifiers(text)))))
        with open(sys.argv[2], "w") as f:
            f.write(result)
    elif len(sys.argv) == 1:
        text = sys.stdin.read()
        sys.stdout.write(fix_stack_pointer(relax_long_jumps(dedup_constants(reorder(sanitize_identifiers(text))))))
    else:
        print("usage: reorder_asm.py [in.s out.s]", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
