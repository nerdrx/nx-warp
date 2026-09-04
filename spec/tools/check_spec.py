#!/usr/bin/env python3
"""Consistency checker for the NX Warp specification in spec/.

Two checks, both cheap and both about the document set rather than the codec:

  1. Pending markers. Every place the specification defers to a component
     document carries a "[pending ...]" marker. They are counted and listed by
     file and by target document, so that "how far from finished is this?" has
     a number rather than a feeling.

  2. Syntax/semantics cross-reference. Clause 04 declares syntax elements in
     fenced ```syntax blocks; clause 05 defines each with a paragraph opening
     **`name`**. Every declared element must be defined, and every definition
     should correspond to a declaration. Both directions are reported.

Exit status is 0 unless --strict is given, in which case any pending marker or
any cross-reference error fails. See spec/README.md for the conventions this
enforces.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# A declaration line inside a ```syntax block: an identifier, optional array
# subscripts, then whitespace, then either a descriptor, "=" for a derived
# element, or a [pending ...] marker standing in for an undecided descriptor.
DECL_RE = re.compile(
    r"^\s{2,}"                       # syntax blocks indent their elements
    r"([A-Za-z_][A-Za-z0-9_]*)"      # the identifier
    r"((?:\[[^\]]*\])*)"             # optional [..] subscripts
    r"\s+"
    r"(f\(|s\(|b\(|ae\(|bp\(|eg3\(|=\s|\[pending)"
)

# A semantics entry: a paragraph opening with **`name`**. One paragraph may
# define several closely related elements, as in "**`width`**, **`height`** are
# the luma dimensions", so every such name on the opening line counts.
DEF_LINE_RE = re.compile(r"^\*\*`[A-Za-z_][A-Za-z0-9_]*`\*\*")
DEF_NAME_RE = re.compile(r"\*\*`([A-Za-z_][A-Za-z0-9_]*)`\*\*")

PENDING_RE = re.compile(r"\[pending([^\]]*)\]")

# A marker inside an inline code span is the document talking *about* the
# notation ("carries a `[pending ...]` marker"), not deferring anything. Those
# spans are removed before the scan so the count stays honest.
CODESPAN_RE = re.compile(r"`[^`]*`")

FENCE_RE = re.compile(r"^```(\w*)\s*$")

# Identifiers that appear in a declaration position but are structure names,
# loop variables or prose rather than syntax elements.
NOT_ELEMENTS = {
    "if", "for", "while", "else", "return", "and", "or",
}


def read(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8").splitlines()


def find_pending(files: list[Path]) -> list[tuple[Path, int, str, str]]:
    """Return (file, lineno, target, line) for every pending marker."""
    out = []
    for path in files:
        for n, line in enumerate(read(path), 1):
            for m in PENDING_RE.finditer(CODESPAN_RE.sub("", line)):
                target = m.group(1).strip() or "(unspecified)"
                out.append((path, n, target, line.strip()))
    return out


def find_declarations(path: Path) -> dict[str, int]:
    """Identifiers declared in ```syntax blocks of `path`, name -> first line."""
    decls: dict[str, int] = {}
    in_syntax = False
    for n, line in enumerate(read(path), 1):
        fence = FENCE_RE.match(line)
        if fence:
            in_syntax = (fence.group(1) == "syntax") if not in_syntax else False
            continue
        if not in_syntax:
            continue
        m = DECL_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        if name in NOT_ELEMENTS:
            continue
        decls.setdefault(name, n)
    return decls


def find_definitions(path: Path) -> dict[str, int]:
    defs: dict[str, int] = {}
    for n, line in enumerate(read(path), 1):
        if not DEF_LINE_RE.match(line):
            continue
        for name in DEF_NAME_RE.findall(line):
            defs.setdefault(name, n)
    return defs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("spec_dir", nargs="?", default=None,
                    help="the spec/ directory (default: the parent of this script's dir)")
    ap.add_argument("--strict", action="store_true",
                    help="exit nonzero if any pending marker or cross-reference error remains")
    ap.add_argument("--quiet", action="store_true", help="print only the summary")
    args = ap.parse_args()

    spec = Path(args.spec_dir) if args.spec_dir else Path(__file__).resolve().parent.parent
    if not spec.is_dir():
        print(f"check_spec: not a directory: {spec}", file=sys.stderr)
        return 2

    files = sorted(p for p in spec.glob("*.md"))
    if not files:
        print(f"check_spec: no .md files in {spec}", file=sys.stderr)
        return 2

    # README.md is the table of contents and the statement of these very
    # conventions: the markers it contains describe the notation and name the
    # component documents, they do not defer any normative behaviour. Scanning
    # it would inflate the count with its own documentation.
    clause_files = [p for p in files if p.name != "README.md"]

    syntax_file = spec / "04-bitstream-syntax.md"
    sem_file = spec / "05-semantics.md"
    missing_clause = [p.name for p in (syntax_file, sem_file) if not p.exists()]

    print(f"NX Warp specification check — {spec}")
    print(f"  {len(clause_files)} clause files (README.md excluded from the pending scan)")
    print()

    # ---- check 1: pending markers -----------------------------------------
    pending = find_pending(clause_files)
    by_target: dict[str, int] = {}
    by_file: dict[str, int] = {}
    for path, _n, target, _line in pending:
        by_target[target] = by_target.get(target, 0) + 1
        by_file[path.name] = by_file.get(path.name, 0) + 1

    print(f"PENDING MARKERS: {len(pending)}")
    if pending:
        print("  by target document:")
        for target, count in sorted(by_target.items(), key=lambda kv: (-kv[1], kv[0])):
            print(f"    {count:3d}  {target}")
        print("  by clause file:")
        for name, count in sorted(by_file.items()):
            print(f"    {count:3d}  {name}")
        if not args.quiet:
            print("  detail:")
            for path, n, target, line in pending:
                snippet = line if len(line) <= 96 else line[:93] + "..."
                print(f"    {path.name}:{n}: [{target}] {snippet}")
    print()

    # ---- check 2: syntax vs semantics -------------------------------------
    undefined: list[tuple[str, int]] = []
    undeclared: list[tuple[str, int]] = []
    if missing_clause:
        print("SYNTAX/SEMANTICS: skipped, missing " + ", ".join(missing_clause))
    else:
        decls = find_declarations(syntax_file)
        defs = find_definitions(sem_file)
        undefined = sorted(((k, v) for k, v in decls.items() if k not in defs),
                           key=lambda kv: kv[1])
        undeclared = sorted(((k, v) for k, v in defs.items() if k not in decls),
                            key=lambda kv: kv[1])

        print(f"SYNTAX/SEMANTICS: {len(decls)} elements declared in "
              f"{syntax_file.name}, {len(defs)} defined in {sem_file.name}")
        if undefined:
            print(f"  ERROR: {len(undefined)} declared but not defined:")
            for name, n in undefined:
                print(f"    {syntax_file.name}:{n}: {name}")
        else:
            print("  ok: every declared element has a semantics entry")
        if undeclared:
            print(f"  WARNING: {len(undeclared)} defined but never declared "
                  f"(prose-only or renamed elements):")
            for name, n in undeclared:
                print(f"    {sem_file.name}:{n}: {name}")
    print()

    # ---- summary -----------------------------------------------------------
    errors = len(undefined) + len(missing_clause)
    print("SUMMARY")
    print(f"  pending markers   : {len(pending)}")
    print(f"  undefined elements: {len(undefined)}")
    print(f"  undeclared entries: {len(undeclared)} (warning only)")
    if errors:
        print("  result            : FAIL")
    elif pending and args.strict:
        print("  result            : FAIL (--strict: pending markers remain)")
    else:
        print("  result            : ok"
              + ("" if not pending else f" ({len(pending)} pending, not a failure without --strict)"))

    if errors:
        return 1
    if args.strict and pending:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
