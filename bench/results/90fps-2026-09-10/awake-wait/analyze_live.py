#!/usr/bin/env python3
"""Summarize WiVRn client/decoder timing windows from captured logs (read-only)."""
import json, re, sys
from pathlib import Path

def ts(s):
    m=re.search(r'(\d\d:\d\d:\d\d\.\d+)',s)
    return m.group(1) if m else None

def num(p,s):
    m=re.search(p,s); return float(m.group(1)) if m else None

def parse(path, limit=30):
    client=[]; dec=[]; c={}; d={}
    for line in Path(path).read_text(errors='replace').splitlines():
        t=ts(line)
        if 'render:' in line:
            m=re.search(r'render: (\d+) iterations in ([0-9.]+) s \(([0-9.]+)/s\)',line)
            if m:
                if c.get('time'): client.append(c)
                c={'time':t,'elapsed_s':float(m.group(2)),'iterations':int(m.group(1)),'iterations_per_s':float(m.group(3))}
                m2=re.search(r'(\d+) new-source',line); c['fresh_per_s']=int(m2.group(1))/float(m.group(2)) if m2 else None
            elif c and 'ready wait attempts' in line:
                c['wait_attempts']=num(r'attempts ([0-9]+)',line)
                c['wait_successes']=num(r'success ([0-9]+)',line)
                c['wait_total_ms']=num(r'total ([0-9.]+) ms',line)
                c['older_available']=num(r'available ([0-9]+)',line)
            elif c and 'own GPU pass' in line: c['own_gpu_ms']=num(r'own GPU pass ([0-9.]+)',line)
            elif c and 'source display-time offset' in line:
                c['source_offset_ms']=num(r'offset ([0-9.-]+)',line); c['submit_lead_ms']=num(r'submit lead ([0-9.-]+)',line)
        if 'nxwarp[' in line:
            m=re.search(r'nxwarp\[\d+\]: (\d+) frames in ([0-9.]+) s:',line)
            if m:
                if d.get('time'): dec.append(d)
                d={'time':t,'elapsed_s':float(m.group(2)),'frames':int(m.group(1)),'frames_per_s':int(m.group(1))/float(m.group(2))}
                d['nxvc_gpu_ms']=num(r'gpu ([0-9.]+) ms',line)
            elif d and 'fence-post' in line and 'nxvc gpu' in line:
                d['fence_post_ms']=num(r'fence-post ([0-9.]+)',line)
                d['copy_gpu_ms']=num(r'copy gpu ([0-9.]+)',line)
                d['queue_ms']=num(r'queue ([0-9.]+)',line)
    if c.get('time'): client.append(c)
    if d.get('time'): dec.append(d)
    def summary(rows, required):
        rows=[r for r in rows if all(isinstance(r.get(k),(int,float)) for k in required)][-limit:]
        keys=sorted({k for r in rows for k in r if k not in ('time','elapsed_s')})
        means={k:sum(r[k] for r in rows if isinstance(r.get(k),(int,float)))/sum(isinstance(r.get(k),(int,float)) for r in rows) for k in keys}
        return {'n':len(rows),'start':rows[0]['time'] if rows else None,'end':rows[-1]['time'] if rows else None,'means':means}
    return {'file':str(path),'client':summary(client,('fresh_per_s','own_gpu_ms','source_offset_ms','submit_lead_ms')),'decoder':summary(dec,('nxvc_gpu_ms','copy_gpu_ms','queue_ms','fence_post_ms'))}

for raw in sys.argv[1:]:
    p=Path(raw)
    print(json.dumps(parse(p),separators=(',',':')))
