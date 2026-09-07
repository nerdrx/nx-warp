#!/usr/bin/env python3
"""Fail-closed preflight for a live WiVRn benchmark capture."""
import argparse, csv, hashlib, json, re, sys
from pathlib import Path

def fail(msg):
    print(f"preflight: FAIL: {msg}", file=sys.stderr)
    raise SystemExit(2)

def text(path):
    try: return Path(path).read_text(errors="replace")
    except OSError as e: fail(f"cannot read {path}: {e}")

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--sha256", required=True)
    ap.add_argument("--log", action="append", required=True, help="startup/scene/server log; repeat")
    ap.add_argument("--banner", action="append", default=[], help="regex required in combined logs; repeat")
    ap.add_argument("--dims", help="regex required in combined logs, e.g. '2160x2160'")
    ap.add_argument("--timings", required=True)
    ap.add_argument("--manifest")
    ap.add_argument("--json", help="write validation result")
    a=ap.parse_args()
    b=Path(a.binary)
    if not b.is_file(): fail(f"binary missing: {b}")
    h=hashlib.sha256(b.read_bytes()).hexdigest()
    if h.lower()!=a.sha256.lower(): fail(f"binary SHA256 {h} != expected {a.sha256}")
    logs='\n'.join(text(p) for p in a.log)
    missing=[pat for pat in a.banner if not re.search(pat,logs,re.MULTILINE)]
    if missing: fail("required runtime banner absent: " + "; ".join(missing))
    if a.dims and not re.search(a.dims,logs): fail(f"required negotiated dimensions absent: {a.dims}")
    t=Path(a.timings)
    if not t.is_file() or t.stat().st_size==0: fail(f"timings CSV missing or empty: {t}")
    rows=t.read_text(errors='replace').splitlines()
    try: parsed=list(csv.reader(rows))
    except csv.Error as e: fail(f"timings CSV is malformed: {e}")
    if not any(len(r) >= 4 and r[0] == 'receive_begin' and r[3] == '0' for r in parsed):
        fail("timings CSV has no stream-0 receive_begin record")
    result={'ok':True,'binary':str(b),'binary_sha256':h,'logs':[str(x) for x in a.log],
            'timings':str(t),'timing_rows':len(rows),'banners':a.banner,'dims_regex':a.dims}
    if a.manifest:
        m=Path(a.manifest)
        if not m.is_file(): fail(f"manifest missing: {m}")
        try: result['manifest']=json.loads(m.read_text())
        except Exception as e: fail(f"manifest is not JSON: {e}")
    if a.json: Path(a.json).write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,separators=(',',':')))

if __name__=='__main__': main()
