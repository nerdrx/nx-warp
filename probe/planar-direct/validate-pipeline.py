#!/usr/bin/env python3
"""Check sync/async RGBA identity for the standalone PLANAR probe."""
import argparse, hashlib, json, math, os, pathlib, re, shutil, subprocess, tempfile

ROW = re.compile(r"^(\d+),.*,(\-?[0-9.eE+]+)(?:,(\-?[0-9.eE+]+))*$")

def run_probe(args, mode, async_run, count, root):
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("NX_PLANAR_"):
            env.pop(key)
    if mode == "compact": env["NX_PLANAR_COMPACT"] = "1"
    if mode == "tile": env["NX_PLANAR_TILE"] = "1"
    if async_run: env["NX_PLANAR_ASYNC"] = "1"
    tag = f"{mode}-{'async' if async_run else 'sync'}-{count}"
    prefix = root / tag
    p = subprocess.run([args.probe, args.fixture, args.shaders, str(count), str(prefix)],
                       env=env, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    (root / (tag + ".log")).write_text(p.stdout)
    if p.returncode:
        raise RuntimeError(f"{tag}: probe failed ({p.returncode})")
    rows = []
    for line in p.stdout.splitlines():
        if not line or not line[0].isdigit(): continue
        fields = line.split(',')
        if len(fields) < 7: raise RuntimeError(f"{tag}: malformed CSV row")
        try: vals = [float(x) for x in fields[1:]]
        except ValueError: raise RuntimeError(f"{tag}: nonnumeric CSV metric")
        if not all(math.isfinite(x) and x >= 0 for x in vals):
            raise RuntimeError(f"{tag}: nonfinite/negative metric")
        rows.append(int(fields[0]))
    if rows != list(range(count)):
        raise RuntimeError(f"{tag}: frame rows {rows!r}, expected 0..{count-1}")
    raw = pathlib.Path(str(prefix) + ".rgba")
    if not raw.is_file(): raise RuntimeError(f"{tag}: missing readback")
    digest = hashlib.sha256(raw.read_bytes()).hexdigest()
    raw.unlink()
    ppm = pathlib.Path(str(prefix) + ".ppm")
    if ppm.exists(): ppm.unlink()
    return {"frames": len(rows), "rgba_sha256": digest}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True); ap.add_argument("--shaders", required=True)
    ap.add_argument("--fixture", required=True); ap.add_argument("--out", required=True)
    args = ap.parse_args(); out = pathlib.Path(args.out); out.mkdir(parents=True, exist_ok=True)
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="planar-pipeline-", dir=out))
    result = {"fixture": args.fixture, "counts": [1, 2, 3, 8], "modes": {}}
    try:
        for mode in ("exact", "compact", "tile"):
            result["modes"][mode] = {}
            for count in result["counts"]:
                sync = run_probe(args, mode, False, count, tmp)
                shutil.copy2(tmp / f"{mode}-sync-{count}.log", out)
                async_ = run_probe(args, mode, True, count, tmp)
                shutil.copy2(tmp / f"{mode}-async-{count}.log", out)
                if sync["rgba_sha256"] != async_["rgba_sha256"]:
                    raise RuntimeError(f"{mode}-{count}: sync/async RGBA mismatch")
                result["modes"][mode][str(count)] = {"sync": sync, "async": async_}
    except Exception as exc:
        result["status"] = "failed"; result["error"] = str(exc)
        (out / "pipeline-validation.json").write_text(json.dumps(result, indent=2) + "\n")
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    result["status"] = "ok"
    (out / "pipeline-validation.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2)); return 0

if __name__ == "__main__": raise SystemExit(main())
