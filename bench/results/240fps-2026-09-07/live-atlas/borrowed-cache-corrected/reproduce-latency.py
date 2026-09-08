#!/usr/bin/env python3
import csv,gzip,json
from collections import defaultdict
from pathlib import Path
import numpy as np
p=Path(__file__).with_name('normalized-timings.csv.gz'); by=defaultdict(lambda:defaultdict(dict))
with gzip.open(p,'rt',newline='') as f:
 for r in csv.DictReader(f):
  if r['stream']=='0':
   d=by[r['arm']][int(r['frame'])]; t=int(r['relative_ns']); d[r['event']]=min(d.get(r['event'],t),t)
out={}
for arm,frames in by.items():
 first=min(v['receive_begin'] for v in frames.values() if 'receive_begin' in v); warm=[v for v in frames.values() if v.get('receive_begin',0)>=first+10_000_000_000]
 vals=[(v['blit']-v['receive_begin'])/1e6 for v in warm if 'receive_begin' in v and 'blit' in v]
 out[arm]={'count':len(vals),'arrival_to_render_selection_ms_p50_p95_p99':np.percentile(vals,[50,95,99]).round(3).tolist()}
print(json.dumps(out,indent=2,sort_keys=True))
