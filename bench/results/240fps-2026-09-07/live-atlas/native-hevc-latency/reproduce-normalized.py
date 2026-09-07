#!/usr/bin/env python3
import csv,gzip,json,subprocess,tempfile,os
from pathlib import Path
D=Path(__file__).resolve().parent
from collections import defaultdict
G=defaultdict(list)
with gzip.open(D/'normalized-timings.csv.gz','rt') as f:
 for r in csv.DictReader(f): G[r['arm']].append([r['event'],r['frame'],r['relative_ns'],r['stream']])
actual={}
for arm,rows in G.items():
 with tempfile.NamedTemporaryFile('w',delete=False) as f: csv.writer(f).writerows(rows); name=f.name
 result=json.loads(subprocess.check_output(['python3',str(D/'summarize-latency.py'),name],text=True)); os.unlink(name); result.pop('file',None); actual[arm]=result
if actual != json.load(open(D/'normalized-verification.json')): raise SystemExit('verification mismatch')
print('verified:', ', '.join(sorted(actual)))
