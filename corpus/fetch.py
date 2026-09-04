#!/usr/bin/env python3
"""Materialise the NX Warp test corpus described by ``corpus/MANIFEST.json``.

The corpus is **not** in the repository: one second of 2048x2048 stereo 4:4:4 at
90 Hz is about 2 GB.  The manifest is in the repository, and this script turns it
into files under the corpus root, then checks their SHA-256 against the manifest.

Three kinds of entry, three behaviours:

``synthetic``
    Generated locally by ``tools/quality/capture/gen_synthetic.py`` (which this
    script drives and never modifies).  Deterministic: the same manifest gives
    byte-identical files on any machine, which is why their hashes can be pinned.
``external``
    A publicly available sequence downloaded over HTTP.  **Never fetched
    automatically** -- ``--download`` is required, every URL is subject to
    ``--max-mb``, and the default run only lists them.
``capture``
    Real frames grabbed from a WiVRn NX session (``tools/quality/README.md`` 1c).
    Nothing to fetch: these are recorded by hand and registered afterwards with
    ``--record``.

Usage
-----
::

    python3 corpus/fetch.py                     # dry run: what exists, what does not
    python3 corpus/fetch.py --sync              # generate the synthetic entries
    python3 corpus/fetch.py --sync --record     # ... and write their hashes back
    python3 corpus/fetch.py --download --max-mb 64
    python3 corpus/fetch.py --only vr-mixed-256 --sync

The corpus root defaults to
``/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/corpus`` and is overridden by
``$NXW_CORPUS`` or ``--root``.  It must never be inside the repository and never
in ``/tmp`` (tmpfs here).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MANIFEST = os.path.join(HERE, "MANIFEST.json")
GENERATOR = os.path.join(REPO, "tools", "quality", "capture", "gen_synthetic.py")
DEFAULT_ROOT = "/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/corpus"

# Reuse the harness's own CPU discipline rather than reimplementing it, so the
# generator lands on the same core slice as everything else.  Optional: the
# script still works from a checkout where tools/quality is absent.
sys.path.insert(0, os.path.join(REPO, "tools", "quality"))
try:
    from nxq import cpu as _cpu  # type: ignore

    def cpu_prefix() -> list[str]:
        return _cpu.prefix()
except Exception:  # pragma: no cover - fallback for a partial checkout
    def cpu_prefix() -> list[str]:
        if os.environ.get("NXQ_NO_CPU_LIMIT") or not shutil.which("chrt"):
            return []
        return ["chrt", "-i", "0", "taskset", "-c",
                os.environ.get("NXQ_CPUS", "28-31"), "nice", "-n", "19"]


# --------------------------------------------------------------------- helpers

def sha256_file(path: str, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            b = fh.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def human(n: int | None) -> str:
    if n is None:
        return "?"
    for unit in ("B", "kB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024.0
    return f"{n} B"


def load_manifest() -> dict:
    with open(MANIFEST) as fh:
        return json.load(fh)


def save_manifest(m: dict) -> None:
    with open(MANIFEST, "w") as fh:
        json.dump(m, fh, indent=2)
        fh.write("\n")


def entry_files(entry: dict) -> list[dict]:
    return entry.get("files", [])


# ------------------------------------------------------------------- synthetic

def generate(entry: dict, root: str, quiet: bool) -> None:
    """Run gen_synthetic.py for one entry.  Its CLI is read, never changed."""
    if not os.path.exists(GENERATOR):
        raise SystemExit(f"generator missing: {GENERATOR}")
    g = entry["generator"]
    cmd = [
        sys.executable, GENERATOR,
        "--out", root,
        "--name", entry["name"],
        "--frames", str(entry["frames"]),
        "--eye-width", str(g["eye_width"]),
        "--eye-height", str(g["eye_height"]),
        "--motion", g["motion"],
        "--layout", g.get("layout", "sbs"),
        "--pix", ",".join(entry["pix_fmt"]),
        "--fps", str(entry["fps"]),
        "--seed", str(g.get("seed", 1)),
    ]
    if "objects" in g:
        cmd += ["--objects", str(g["objects"])]
    if "pano_width" in g:
        cmd += ["--pano-width", str(g["pano_width"])]
    if "hfov" in g:
        cmd += ["--hfov", str(g["hfov"])]
    if "vfov" in g:
        cmd += ["--vfov", str(g["vfov"])]
    if g.get("no_hud"):
        cmd.append("--no-hud")
    # Version 1 material: the generator reproduces it bit for bit under
    # --legacy, which is what keeps every number published on these entries
    # reproducible after the band-limiting change (docs/WARP-AUDIT.md 4,
    # tools/quality/reports/gates-v2-2026-09-04.md).
    if g.get("legacy"):
        cmd.append("--legacy")
    elif "supersample" in g:
        cmd += ["--supersample", str(g["supersample"])]
    # --full is the generator's own guard against filling the disk with
    # accidental multi-gigabyte renders.  Pass it only when the manifest says
    # the entry really is that big.
    if g["eye_width"] * g["eye_height"] > 1_000_000:
        cmd.append("--full")
    if quiet:
        cmd.append("--quiet")

    subprocess.run(cpu_prefix() + cmd, check=True)


# -------------------------------------------------------------------- external

def download(entry: dict, root: str, max_mb: float) -> bool:
    """Fetch one external entry.  Returns True if anything was written."""
    wrote = False
    for f in entry_files(entry):
        url = f.get("url")
        if not url:
            print(f"  {f['path']}: no url in the manifest, skipping")
            continue
        dest = os.path.join(root, f["path"])
        if os.path.exists(dest):
            print(f"  {f['path']}: already present")
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)

        # Size check BEFORE the body: a HEAD tells us whether this blows the cap
        # without spending the bandwidth to find out.
        size = f.get("bytes")
        try:
            req = urllib.request.Request(url, method="HEAD")
            with urllib.request.urlopen(req, timeout=30) as r:
                cl = r.headers.get("Content-Length")
                if cl:
                    size = int(cl)
        except Exception as e:
            print(f"  {f['path']}: HEAD failed ({e}); relying on the manifest size")

        if size is None:
            print(f"  {f['path']}: unknown size and no manifest size, refusing "
                  f"(pass an explicit --max-mb and a 'bytes' field to override)")
            continue
        if size > max_mb * 1024 * 1024:
            print(f"  {f['path']}: {human(size)} exceeds --max-mb {max_mb:g}, skipping")
            continue

        print(f"  {f['path']}: downloading {human(size)} from {url}")
        tmp = dest + ".part"
        try:
            with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as out:
                shutil.copyfileobj(r, out, 1 << 20)
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            print(f"  {f['path']}: FAILED ({e})")
            if os.path.exists(tmp):
                os.remove(tmp)
            continue
        os.replace(tmp, dest)
        wrote = True
    return wrote


# ---------------------------------------------------------------------- verify

def check_entry(entry: dict, root: str, record: bool) -> tuple[int, int, int]:
    """Returns (present, missing, mismatched) for one entry's files."""
    present = missing = bad = 0
    for f in entry_files(entry):
        path = os.path.join(root, f["path"])
        if not os.path.exists(path):
            missing += 1
            continue
        present += 1
        digest = sha256_file(path)
        size = os.path.getsize(path)
        want = f.get("sha256")
        if record:
            f["sha256"] = digest
            f["bytes"] = size
        elif want and want != digest:
            bad += 1
            print(f"  MISMATCH {f['path']}")
            print(f"    manifest {want}")
            print(f"    on disk  {digest}  ({human(size)})")
        elif not want:
            print(f"  {f['path']}: no hash in the manifest "
                  f"(run with --record to pin {digest[:16]}...)")
    return present, missing, bad


# ------------------------------------------------------------------------ main

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Materialise and verify the NX Warp test corpus.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Default is a dry run: nothing is generated and nothing is downloaded.",
    )
    ap.add_argument("--root", default=os.environ.get("NXW_CORPUS", DEFAULT_ROOT),
                    help="corpus root (default $NXW_CORPUS or the scratch volume)")
    ap.add_argument("--sync", action="store_true",
                    help="generate the synthetic entries that are missing")
    ap.add_argument("--force", action="store_true",
                    help="with --sync, regenerate entries that already exist")
    ap.add_argument("--download", action="store_true",
                    help="fetch external entries (never happens without this)")
    ap.add_argument("--max-mb", type=float, default=64.0,
                    help="per-file download cap in MB (default 64)")
    ap.add_argument("--record", action="store_true",
                    help="write the hashes and sizes of present files back into "
                         "MANIFEST.json (use when adding an entry)")
    ap.add_argument("--only", action="append", default=[],
                    help="restrict to these entry names (repeatable)")
    ap.add_argument("--klass", "--class", dest="klass", action="append", default=[],
                    help="restrict to these classes (repeatable)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    m = load_manifest()
    root = os.path.abspath(args.root)

    # A corpus inside the repo would be committed by accident; a corpus in /tmp
    # is on tmpfs here and would eat RAM.  Refuse both.
    if os.path.commonpath([root, os.path.abspath(REPO)]) == os.path.abspath(REPO):
        return fail(f"corpus root {root} is inside the repository; use $NXW_CORPUS")
    if root == "/tmp" or root.startswith("/tmp/"):
        return fail(f"corpus root {root} is on tmpfs; use the scratch volume")

    entries = m["entries"]
    if args.only:
        entries = [e for e in entries if e["name"] in args.only]
    if args.klass:
        entries = [e for e in entries if e["class"] in args.klass]
    if not entries:
        return fail("no entries selected")

    os.makedirs(root, exist_ok=True)
    print(f"corpus root: {root}")
    print(f"manifest:    {MANIFEST} (schema {m['schema']}, {len(m['entries'])} entries)")
    if not (args.sync or args.download or args.record):
        print("mode:        DRY RUN (pass --sync to generate, --download to fetch)\n")
    else:
        print()

    tot_present = tot_missing = tot_bad = 0
    tot_bytes = 0

    for e in entries:
        kind = e["kind"]
        n_files = len(entry_files(e))
        want_bytes = sum(f.get("bytes") or 0 for f in entry_files(e))
        print(f"{e['name']}  [{kind}/{e['class']}]  {e['resolution']}  "
              f"{e['frames']} frames @ {e['fps']}  {'+'.join(e['pix_fmt'])}  "
              f"{n_files} file(s), {human(want_bytes) if want_bytes else '? bytes'}")
        if e.get("note"):
            print(f"  note: {e['note']}")

        have_all = all(os.path.exists(os.path.join(root, f["path"]))
                       for f in entry_files(e))

        if kind == "synthetic" and args.sync and (args.force or not have_all):
            print("  generating...")
            try:
                generate(e, root, args.quiet)
            except subprocess.CalledProcessError as exc:
                print(f"  GENERATOR FAILED (exit {exc.returncode})")
                tot_bad += 1
                continue
        elif kind == "external":
            if args.download:
                if e.get("url_verified") is False:
                    print("  (URL not verified by the author -- if this 404s, the "
                          "sequence moved; see corpus/README.md)")
                download(e, root, args.max_mb)
            elif not have_all:
                for f in entry_files(e):
                    print(f"  would fetch {f['path']} "
                          f"({human(f.get('bytes'))}) from {f.get('url', '(no url)')}")
        elif kind == "capture" and not have_all:
            print("  not fetchable: record it per tools/quality/README.md 1c, "
                  "then re-run with --record")

        p, miss, bad = check_entry(e, root, args.record)
        tot_present += p
        tot_missing += miss
        tot_bad += bad
        for f in entry_files(e):
            path = os.path.join(root, f["path"])
            if os.path.exists(path):
                tot_bytes += os.path.getsize(path)
        print(f"  {p} present, {miss} missing, {bad} mismatched")
        print()

    if args.record:
        save_manifest(m)
        print(f"wrote hashes into {MANIFEST}")

    print(f"total: {tot_present} file(s) present ({human(tot_bytes)}), "
          f"{tot_missing} missing, {tot_bad} mismatched")
    if tot_bad:
        return 1
    if tot_present == 0:
        # ctest's "skipped": a machine with no corpus is not a broken machine.
        print("nothing materialised; run with --sync")
        return 77
    return 0


def fail(msg: str) -> int:
    print(f"error: {msg}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
