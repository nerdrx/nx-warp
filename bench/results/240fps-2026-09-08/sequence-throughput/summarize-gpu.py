#!/usr/bin/env python3
import argparse, csv, gzip, json, math
from pathlib import Path

def percentile(values, p):
    values = sorted(values)
    return values[max(0, min(len(values)-1, math.ceil(p*len(values))-1))]

def summarize(path):
    with gzip.open(path, 'rt', newline='') as f:
        rows = [r for r in csv.DictReader(f) if int(r['frame']) >= 120]
    out = {'file': path.name, 'steady_rows': len(rows)}
    for key in ('gpu_ms', 'total_ms', 'render_ms'):
        vals = [float(r[key]) for r in rows if r.get(key, '')]
        if not vals:
            continue
        out[key] = {'p50': percentile(vals,.50), 'p95': percentile(vals,.95), 'p99': percentile(vals,.99)}
    return out

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--directory', type=Path, default=Path(__file__).parent)
    ap.add_argument('--output', type=Path, default=None)
    a=ap.parse_args()
    result=[summarize(a.directory/f'sequence-gpu-{arm}.csv.gz') for arm in ('control','gpu','restored')]
    text=json.dumps(result, indent=2)+'\n'
    if a.output: a.output.write_text(text)
    else: print(text, end='')
if __name__=='__main__': main()
