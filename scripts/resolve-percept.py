#!/usr/bin/env python3
"""Resolve the two conflicts merging tourney/percept onto the tournament tree.

`tools/quality/nxq/fvvdp.py` is an **add/add** conflict. `tourney/percept`
forked before `tourney/metric` landed on main and carries a verbatim copy of
metric's file so its RESULTS numbers came from the same code. The branch's own
header says so:

    BORROWED, UNMODIFIED, from branch `tourney/metric` ... If the two branches
    are merged this file is a duplicate and the `tourney/metric` copy is the
    one to keep.

main already merged `tourney/metric`, so **main's copy wins** and the branch's
is dropped. Taking the branch's side instead would silently revert whatever
metric has changed since the copy was taken.

`tools/quality/README.md` conflicts twice, and both are purely additive: the
tool table gains percept's three scripts alongside main's `fvvdp`/`popin`/
`latency` rows, and percept's `percept_run.py` section sits after main's
`--metric` section. The resolution is the union, main's side first.

Idempotent: a tree with no conflict markers is left alone.
"""

import os
import re
import subprocess
import sys

FVVDP = "tools/quality/nxq/fvvdp.py"
README = "tools/quality/README.md"


def sh(*args):
    return subprocess.run(args, capture_output=True, text=True).returncode


def conflicted():
    out = subprocess.run(["git", "diff", "--name-only", "--diff-filter=U"],
                         capture_output=True, text=True, check=True).stdout
    return [p for p in out.split("\n") if p]


def main() -> int:
    todo = conflicted()

    if FVVDP in todo:
        # --ours is the tree we are merging INTO, i.e. main's (metric's) copy.
        if sh("git", "checkout", "--ours", "--", FVVDP) != 0:
            print("resolve-percept: could not take main's %s" % FVVDP,
                  file=sys.stderr)
            return 1
        sh("git", "add", "--", FVVDP)
        print("resolve-percept: kept main's %s, dropped the branch copy" % FVVDP)

    if README in todo and os.path.exists(README):
        with open(README, encoding="utf-8") as f:
            text = f.read()
        if "<<<<<<<" in text:
            # union: both sides, ours first, markers removed
            text, n = re.subn(
                r"<<<<<<< [^\n]*\n(.*?)\n?=======\n(.*?)>>>>>>> [^\n]*\n",
                lambda m: m.group(1) + "\n" + m.group(2),
                text, flags=re.S)
            if "<<<<<<<" in text:
                print("resolve-percept: %s still has markers; resolve by hand"
                      % README, file=sys.stderr)
                return 1
            with open(README, "w", encoding="utf-8") as f:
                f.write(text)
            print("resolve-percept: unioned %d conflict(s) in %s" % (n, README))
        sh("git", "add", "--", README)

    left = conflicted()
    if left:
        print("resolve-percept: still conflicted: %s" % " ".join(left),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
