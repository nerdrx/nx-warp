#!/usr/bin/env python3
"""Rebuild checker-summary.csv from adjacent JSON log and immutable metadata."""
import csv
import json
import re
import statistics
from pathlib import Path

root = Path(__file__).resolve().parent
meta = json.loads((root / 'checker-metadata.json').read_text())
log = (root / 'checker-bench-runs.txt').read_text()
expected = ['baseline', 'candidate', 'candidate', 'baseline'] * 3
runs = []
for i, (build, body) in enumerate(re.findall(r'RUN \d+ (baseline|fast)\n(\{.*?\n\})', log, re.S), 1):
    result = json.loads(body)
    t = result['timing']['all_phases']
    runs.append({'run': i, 'build': 'candidate' if build == 'fast' else build,
                 'mean_ms': t['mean_ms'], 'p50_ms': t['p50_ms'], 'p95_ms': t['p95_ms'],
                 'output_bytes': result['merged_output_bytes'], 'checksum': result['checksum']})
if len(runs) != 12 or [r['build'] for r in runs] != expected:
    raise SystemExit('expected twelve runs in ABBA repeated three times')
if len({r['checksum'] for r in runs}) != 1 or len({r['output_bytes'] for r in runs}) != 1:
    raise SystemExit('baseline and candidate outputs differ')
if not meta.get('checksums_identical') or not meta.get('candidate_header_sha256'):
    raise SystemExit('checker metadata is incomplete')
for actual, stored in zip(runs, meta['runs']):
    if actual['build'] != stored['build'] or actual['checksum'] != stored['checksum']:
        raise SystemExit('saved log does not match immutable run metadata')

def median(build, key):
    return statistics.median(r[key] for r in runs if r['build'] == build)
p50_delta = 100 * (median('candidate', 'p50_ms') / median('baseline', 'p50_ms') - 1)
p95_delta = 100 * (median('candidate', 'p95_ms') / median('baseline', 'p95_ms') - 1)
with (root / 'checker-summary.csv').open('w', newline='') as f:
    w = csv.writer(f, lineterminator="\n")
    w.writerow(['record', 'run', 'build', 'mean_ms', 'p50_ms', 'p95_ms', 'p50_change_vs_baseline_pct', 'p95_change_vs_baseline_pct', 'output_bytes', 'checksum'])
    for r in runs:
        w.writerow(['run', r['run'], r['build'], f"{r['mean_ms']:.6f}", f"{r['p50_ms']:.6f}", f"{r['p95_ms']:.6f}", '', '', r['output_bytes'], r['checksum']])
    for build in ('baseline', 'candidate'):
        w.writerow(['median', '', build, '', f"{median(build, 'p50_ms'):.6f}", f"{median(build, 'p95_ms'):.6f}", '', '', '', ''])
    w.writerow(['change_percent', '', 'candidate_vs_baseline', '', '', '', f'{p50_delta:.2f}', f'{p95_delta:.2f}', '', ''])
print(f"p50 median {median('baseline','p50_ms'):.6f} -> {median('candidate','p50_ms'):.6f} ms ({p50_delta:.2f}%)")
print(f"p95 median {median('baseline','p95_ms'):.6f} -> {median('candidate','p95_ms'):.6f} ms ({p95_delta:.2f}%)")
