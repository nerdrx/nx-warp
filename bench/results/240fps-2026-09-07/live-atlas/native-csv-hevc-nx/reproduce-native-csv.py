#!/usr/bin/env python3
import csv,gzip,json
from collections import defaultdict
from pathlib import Path
import numpy as np
p=Path(__file__).with_name('normalized-timings.csv.gz'); by=defaultdict(lambda:defaultdict(dict))
with gzip.open(p,'rt',newline='') as f:
 for r in csv.DictReader(f):
  if r['stream']=='0': d=by[r['arm']][int(r['frame'])]; t=int(r['relative_ns']); d[r['event']]=min(d.get(r['event'],t),t)
out={}
for arm,frames in by.items():
 first=min(v['receive_begin'] for v in frames.values() if 'receive_begin' in v); warm=[v for v in frames.values() if v.get('receive_begin',0)>=first+10_000_000_000]
 stages={}
 for name,a,b in [('arrival_to_ready','receive_begin','decode_end'),('selection','receive_begin','blit')]:
  vals=[(v[b]-v[a])/1e6 for v in warm if a in v and b in v]; stages[name]={'count':len(vals),'p50_p95_p99_ms':np.percentile(vals,[50,95,99]).round(3).tolist()}
 out[arm]={'received_frames':len(warm),'selected_frames':stages['selection']['count'],'stages':stages}
print(json.dumps(out,indent=2,sort_keys=True))
