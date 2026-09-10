#!/usr/bin/env python3
"""Frame-weighted post-warm render-window summary for static-bleed trials."""
import json,re,sys
from pathlib import Path
def sec(s):
 m=re.search(r'(\d\d):(\d\d):(\d\d\.\d+)',s)
 return float(m.group(1))*3600+float(m.group(2))*60+float(m.group(3)) if m else None
def parse(path):
 lines=Path(path).read_text(errors='replace').splitlines(); ts=[sec(x) for x in lines if re.search(r'render: \d+ iterations',x)]; first=next((x for x in ts if x is not None),0); rows=[]; cur=None
 for line in lines:
  t=sec(line)
  if t is not None and (t-first)%86400<10: continue
  if 'render:' in line:
   m=re.search(r'render: (\d+) iterations in ([0-9.]+) s \(([0-9.]+)/s\)',line)
   if m:
    cur={'frames':int(m.group(1)),'seconds':float(m.group(2)),'fresh':0,'gpu':None,'offset':None}
    n=re.search(r'(\d+) new-source',line); cur['fresh']=int(n.group(1)) if n else 0; rows.append(cur)
  if cur:
   m=re.search(r'own GPU pass ([0-9.]+) ms',line)
   if m: cur['gpu']=float(m.group(1))
   m=re.search(r'source display-time offset ([0-9.]+) ms',line)
   if m: cur['offset']=float(m.group(1))
 def summary(rs):
  rs=[r for r in rs if r['gpu'] is not None and r['offset'] is not None]
  total=sum(r['frames'] for r in rs)
  w=lambda k: sum(r[k]*r['frames'] for r in rs)/total if total else None
  return {'windows':len(rs),'frames':total,'reported_seconds':sum(r['seconds'] for r in rs),'fresh_frames':sum(r['fresh'] for r in rs),'nonfresh_iterations':sum(r['frames']-r['fresh'] for r in rs),'gpu_ms_frame_weighted':w('gpu'),'source_offset_ms_frame_weighted':w('offset')}
 return {'file':str(path),'warmup_cutoff_s':10,'all_windows':summary(rows),'windows_over_100_frames':summary([r for r in rows if r['frames']>100])}
for p in sys.argv[1:]: print(json.dumps(parse(p),separators=(',',':')))
