#!/usr/bin/env python3
"""Apply the docs/TOOLBITS.md tool-bit allocation to a merged tree.

Every tournament branch put its first new tool on bit 24, because 24 was the
next free bit on `main` for all of them.  After a merge the definitions
collide; this script moves each tool to its allocated slot.

It is cheap because the C reference and the python package both refer to tools
by *name* everywhere except their definition sites, so a renumber touches
exactly four files:

    include/nxvc/nxvc.h        NXVC_TOOL_<NAME>, NXVC_BITSTREAM_MINOR
    python/src/nxvc/_ffi.py    Tool.<NAME>, the names table, Tool.RESERVED_FROM,
                               NXVC_BITSTREAM_MINOR
    python/src/nxvc/bitstream.py   (names only -- checked, not rewritten)
    docs/SYNTAX.md             the section 2.3 table rows

`ref/src/*` needs no edit at all for a renumber, because it names tools rather
than numbering them.

**`tests/ref/vectors.cpp` is the exception, and this script cannot fix it.**
The reject vectors build malformed headers by poking raw bytes, e.g.

    b[32 + 3] |= 0x01;            // tool bit 24 NEAR_SKIP

which is byte 3 of the u64 `tools` field -- bit 24 spelled as a literal. After
a renumber that line still sets bit 24, which is now a different tool or no
tool at all, so the decoder answers `VERSION` ("unsupported tool") where the
vector expects `BITSTREAM`. `check_literal_pokes()` below reports every such
line; each one has to be rewritten against the constant by hand.

Two branches ship the same tool under a different name; those are unified onto
one name first, so the slot is allocated once:

    inter-b WARP_DC  -> NEAR_SKIP     (inter-a's name)
    inter-b MV_QUAD  -> QUAD_MV       (inter-a's name)

Idempotent: running it on an already-renumbered tree is a no-op.
"""

import os
import re
import subprocess
import sys

MINOR = 6

# docs/TOOLBITS.md section 2.  Tools absent from the tree are skipped.
ALLOC = {
    # JUDGE-detail.md landed first and fixes detail-a's two bits, so detail
    # merges first and every later package renumbers around 19 and 24.
    "XFORM_4X4_SPLIT": 19,   # detail-a, pre-declared on merge-main
    "INTRA_CFL":       24,   # detail-a
    "CTX_V3":          25,   # ctx
    "VEC_ENT":         26,   # ctx-a's second tool
    "TAB_V2":          26,   # ctx-b's second tool (only one ctx branch wins)
    "XFORM_LARGE":     27,   # xform
    "NEAR_SKIP":       28,   # inter
    "QUAD_MV":         29,   # inter
    "SUBTILE_INTRA":   30,   # inter-a only
    "TILE_EXT":        31,   # inter-a only, option A
}

# Names that mean the same tool.  Applied across the whole tree before the
# renumber, so inter-b merges onto inter-a's slot rather than a new one.
ALIASES = {
    "WARP_DC": "NEAR_SKIP",
    "MV_QUAD": "QUAD_MV",
}

NXVC_H = "include/nxvc/nxvc.h"
FFI_PY = "python/src/nxvc/_ffi.py"
BITS_PY = "python/src/nxvc/bitstream.py"
SYNTAX = "docs/SYNTAX.md"

changed: list[str] = []


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def write(p, text, before):
    if text != before:
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)
        changed.append(p)


# Files that *talk about* the rename rather than perform it.  Renaming inside
# them turns "inter-b's WARP_DC is renamed to NEAR_SKIP" into "inter-b's
# NEAR_SKIP is renamed to NEAR_SKIP" and, in this file's own case, collapses
# ALIASES to {"NEAR_SKIP": "NEAR_SKIP"} -- the script quietly eats its own
# mapping and the documentation of why the bits moved.  Learned the hard way.
ALIAS_EXEMPT = {
    "docs/TOOLBITS.md",
    "docs/MERGE-PLAN.md",
    "scripts/retool-bits.py",
}


def tracked_files():
    out = subprocess.run(["git", "ls-files"], capture_output=True, text=True,
                         check=True).stdout.split("\n")
    keep = (".h", ".c", ".cc", ".cpp", ".inc", ".py", ".md", ".comp")
    return [p for p in out
            if p.endswith(keep) and os.path.exists(p) and p not in ALIAS_EXEMPT]


def apply_aliases():
    """Unify the duplicate tool names across the tree.

    Only identifiers are rewritten -- ``NXVC_TOOL_<NAME>`` in C and
    ``Tool.<NAME>`` in python.  A bare ``WARP_DC`` in prose is left alone: it
    is as likely to be a sentence about the rename as the symbol itself.
    """
    live = {a: b for a, b in ALIASES.items() if a != b}
    if not live:
        return
    alt = "|".join(map(re.escape, live))
    for path in tracked_files():
        before = read(path)
        text = re.sub(r"\bNXVC_TOOL_(%s)\b" % alt,
                      lambda m: "NXVC_TOOL_" + live[m.group(1)], before)
        text = re.sub(r"\bTool\.(%s)\b" % alt,
                      lambda m: "Tool." + live[m.group(1)], text)
        write(path, text, before)


def present(text, name):
    return re.search(r"\bNXVC_TOOL_%s\b" % name, text) is not None


def patch_nxvc_h():
    before = read(NXVC_H)
    text = before
    for name, bit in ALLOC.items():
        text = re.sub(
            r"(#define\s+NXVC_TOOL_%s\s+\(1ull\s*<<\s*)\d+(\s*\))" % name,
            lambda m, b=bit: "%s%d%s" % (m.group(1), b, m.group(2)),
            text)
    text = re.sub(r"(#define\s+NXVC_BITSTREAM_MINOR\s+)\d+",
                  lambda m: m.group(1) + str(MINOR), text)
    write(NXVC_H, text, before)
    return text


def patch_ffi(alloc_present):
    before = read(FFI_PY)
    text = before
    for name, bit in ALLOC.items():
        # Tool.<NAME> = 1 << N
        text = re.sub(r"(^\s*%s\s*=\s*1\s*<<\s*)\d+" % name,
                      lambda m, b=bit: m.group(1) + str(b), text, flags=re.M)
        # (1 << N, "NAME") in the names table
        text = re.sub(r"\(1\s*<<\s*\d+,\s*\"%s\"\)" % name,
                      '(1 << %d, "%s")' % (bit, name), text)
    # The first reserved bit sits above *every* allocated bit, not just the
    # tournament ones: 23 FILTER_CATMULL_ROM is allocated on merge-main even
    # though it is reject-in-v1.  Taking max() over the tournament tools alone
    # yields RESERVED_FROM = 20 on a tree that has only bit 19, which would
    # make the decoder reject WM_ID, CTX_V2 and SIGN_HIDE as reserved.
    reserved = max(max(alloc_present.values()), 23) + 1
    text = re.sub(r"(RESERVED_FROM\s*=\s*)\d+",
                  lambda m: m.group(1) + str(reserved), text)
    text = re.sub(r"(^NXVC_BITSTREAM_MINOR\s*=\s*)\d+",
                  lambda m: m.group(1) + str(MINOR), text, flags=re.M)
    write(FFI_PY, text, before)
    return reserved


def patch_syntax(alloc_present):
    """Renumber the section 2.3 table rows: | N | `NAME` | ... |"""
    if not os.path.exists(SYNTAX):
        return
    before = read(SYNTAX)
    text = before
    for name, bit in alloc_present.items():
        text = re.sub(r"^\|\s*\d+\s*\|\s*`%s`\s*\|" % name,
                      "| %d | `%s` |" % (bit, name), text, flags=re.M)
    write(SYNTAX, text, before)


VECTORS_CPP = "tests/ref/vectors.cpp"


def check_literal_pokes():
    """Report reject vectors that spell a tool bit as a raw byte offset.

    These do not survive a renumber; see the module docstring.  Reported, never
    rewritten: the correct constant depends on what the vector means to test.
    """
    if not os.path.exists(VECTORS_CPP):
        return []
    bad = []
    for i, ln in enumerate(read(VECTORS_CPP).splitlines(), 1):
        if re.search(r"b\[\s*32\s*\+\s*\d+\s*\]\s*\|=", ln):
            bad.append("%s:%d: %s" % (VECTORS_CPP, i, ln.strip()))
        # A vector's own description that names a tool bit by number goes stale
        # the same way, and it is what the conformance record says the vector
        # tests.  Name the tool, not the number.
        elif re.search(r'"[^"]*tool bit \d+', ln):
            bad.append("%s:%d: stale prose: %s" % (VECTORS_CPP, i, ln.strip()))
    return bad


def main():
    if not os.path.exists(NXVC_H):
        print("retool-bits: run me from the top of the worktree", file=sys.stderr)
        return 2

    apply_aliases()

    header = read(NXVC_H)
    alloc_present = {n: b for n, b in ALLOC.items() if present(header, n)}
    if not alloc_present:
        print("retool-bits: no tournament tools in this tree, nothing to do")
        return 0

    if len(set(alloc_present.values())) != len(alloc_present):
        dupes = {}
        for n, b in alloc_present.items():
            dupes.setdefault(b, []).append(n)
        clash = {b: ns for b, ns in dupes.items() if len(ns) > 1}
        print("retool-bits: two tools allocated the same bit: %r" % clash,
              file=sys.stderr)
        print("  ctx-a's VEC_ENT and ctx-b's TAB_V2 share slot 27 by design;",
              file=sys.stderr)
        print("  only one ctx branch may win.  Fix docs/TOOLBITS.md if both are here.",
              file=sys.stderr)
        return 1

    patch_nxvc_h()
    reserved = patch_ffi(alloc_present)
    patch_syntax(alloc_present)

    # bitstream.py refers to tools by name only; assert that stays true.
    # Match only shifts on a line that also names a tool: bitstream.py has
    # `1 << 30` and `1 << 31` as warp-matrix numeric ranges, which are not
    # tool bits and must not be flagged.
    if os.path.exists(BITS_PY):
        stray = [ln.strip() for ln in read(BITS_PY).splitlines()
                 if re.search(r"1\s*<<\s*(2[4-9]|3[0-9])", ln)
                 and re.search(r"\bTool\b|tool bit", ln)]
        if stray:
            print("retool-bits: WARNING %s has raw tool-bit literals %r; "
                  "check them by hand" % (BITS_PY, stray), file=sys.stderr)

    pokes = check_literal_pokes()
    if pokes:
        print("retool-bits: WARNING these reject vectors poke a tool bit as a raw",
              file=sys.stderr)
        print("  byte and will NOT survive the renumber (expect VERSION where the",
              file=sys.stderr)
        print("  vector wants BITSTREAM).  Rewrite each against the constant:",
              file=sys.stderr)
        for l in pokes:
            print("    " + l, file=sys.stderr)

    for n, b in sorted(alloc_present.items(), key=lambda kv: kv[1]):
        print("   bit %-2d  %s" % (b, n))
    print("   RESERVED_FROM = %d, syntax v1.%d" % (reserved, MINOR))
    print("retool-bits: rewrote %s" % (", ".join(changed) if changed
                                       else "nothing (already applied)"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
