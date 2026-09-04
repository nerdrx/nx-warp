#!/usr/bin/env python3
"""Check the materialised corpus against ``corpus/MANIFEST.json``.

``fetch.py`` verifies as it goes; this is the standalone check, cheap enough to
run before a measurement campaign and safe to wire into CI.  It never generates,
never downloads and never writes.

::

    python3 corpus/verify.py                   # everything present
    python3 corpus/verify.py --only vr-mixed-256
    python3 corpus/verify.py --strict          # missing files are failures too

Exit codes:

===  ===========================================================
0    every present file matches its recorded hash
1    at least one file's content does not match the manifest
2    bad arguments, or a corpus root that must not be used
77   nothing is materialised -- ctest's "skipped", not a failure
===  ===========================================================

The 77 matters: a developer machine with no corpus is not a broken machine, and
a corpus test that goes red on a fresh checkout trains people to ignore red.

A mismatch, on the other hand, is never benign.  Synthetic entries are
deterministic by construction (`gen_synthetic.py` produces byte-identical output
from the same arguments on any machine, down to its own built-in bitmap font),
so a changed hash means one of three things, in decreasing order of likelihood:

1. the generator changed and the manifest was not re-recorded -- re-run
   ``fetch.py --sync --force --record`` and **commit the hash change as its own
   commit**, because it invalidates every quality number measured before it;
2. the file was truncated or partially written;
3. the generator is not deterministic after all, which is a bug in it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MANIFEST = os.path.join(HERE, "MANIFEST.json")
DEFAULT_ROOT = "/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/corpus"


def sha256_file(path: str, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            b = fh.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description="Verify the NX Warp test corpus.")
    ap.add_argument("--root", default=os.environ.get("NXW_CORPUS", DEFAULT_ROOT))
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--klass", "--class", dest="klass", action="append", default=[])
    ap.add_argument("--strict", action="store_true",
                    help="treat missing files as failures")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    if os.path.commonpath([root, os.path.abspath(REPO)]) == os.path.abspath(REPO):
        print(f"error: corpus root {root} is inside the repository", file=sys.stderr)
        return 2

    with open(MANIFEST) as fh:
        m = json.load(fh)

    entries = m["entries"]
    if args.only:
        entries = [e for e in entries if e["name"] in args.only]
    if args.klass:
        entries = [e for e in entries if e["class"] in args.klass]
    if not entries:
        print("error: no entries selected", file=sys.stderr)
        return 2

    ok = missing = bad = unpinned = 0
    total_bytes = 0

    for e in entries:
        for f in e.get("files", []):
            path = os.path.join(root, f["path"])
            if not os.path.exists(path):
                missing += 1
                if not args.quiet:
                    print(f"missing  {e['name']}: {f['path']}")
                continue
            size = os.path.getsize(path)
            total_bytes += size
            want = f.get("sha256")
            if not want:
                unpinned += 1
                print(f"UNPINNED {e['name']}: {f['path']} has no hash in the "
                      f"manifest (run fetch.py --record)")
                continue
            # Size is checked first: it is free and it names the failure better
            # than a hash mismatch does.
            if f.get("bytes") is not None and size != f["bytes"]:
                bad += 1
                print(f"SIZE     {e['name']}: {f['path']} is {size} bytes, "
                      f"manifest says {f['bytes']}")
                continue
            got = sha256_file(path)
            if got != want:
                bad += 1
                print(f"MISMATCH {e['name']}: {f['path']}")
                print(f"           manifest {want}")
                print(f"           on disk  {got}")
            else:
                ok += 1
                if not args.quiet:
                    print(f"ok       {e['name']}: {f['path']}  ({size} bytes)")

    print(f"\n{ok} ok, {missing} missing, {bad} mismatched, {unpinned} unpinned "
          f"({total_bytes / 1e6:.1f} MB checked)  root={root}")

    if bad or unpinned:
        return 1
    if missing and args.strict:
        print("--strict: missing files are failures")
        return 1
    if ok == 0:
        print("corpus not materialised; run: python3 corpus/fetch.py --sync")
        return 77
    return 0


if __name__ == "__main__":
    sys.exit(main())
