#!/usr/bin/env python3
"""Reproduce profile and mapped pipeline percentiles from archived captures."""
import csv, gzip, importlib.util, json, re
from pathlib import Path
root = Path(__file__).resolve().parent
repo = next(p for p in root.parents if (p/'tools/summarize_pipeline_latency.py').exists())
spec = importlib.util.spec_from_file_location('pipeline', repo/'tools/summarize_pipeline_latency.py')
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)
result = []
for arm in ['baseline', 'optimized', 'restored']:
    with gzip.open(root/f'{arm}-profile.log.gz', 'rt') as f:
        values = [float(v) for v in re.findall(r'nxe:.* total ([0-9.]+) ms', f.read())]
    profile = {'frames':len(values), 'p50_p95_p99_ms': [round(pipeline.percentile(values, q),3) for q in [.5,.95,.99]],
               'above_4_167_ms_percent':round(100*sum(v>1000/240 for v in values)/len(values),3)}
    with gzip.open(root/f'{arm}.csv.gz','rt') as f:
        timing = pipeline.summarize(csv.reader(f), 'nx', 10)
    result.append({'arm':arm, 'profile_all_frames':profile, 'mapped_pipeline':timing})
print(json.dumps(result,indent=2))
