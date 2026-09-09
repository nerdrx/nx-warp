#!/usr/bin/env python3
"""Read-only summary of post-warm FDM/ready-wait client trials."""
import json, re, sys
from pathlib import Path

def seconds(line):
    m = re.search(r'(\d\d):(\d\d):(\d\d\.\d+)', line)
    return float(m.group(1))*3600 + float(m.group(2))*60 + float(m.group(3)) if m else None

def run(path):
    lines = Path(path).read_text(errors='replace').splitlines()
    times = [seconds(x) for x in lines if re.search(r'render: \d+ iterations', x)]
    start = times[0] if times else 0.0
    mtp=[]; gpu=[]; late=[]; repeats=[]; fresh=[]; waits=[]; offsets=[]
    for line in lines:
        t=seconds(line)
        if t is None or (t-start)%86400 < 10: continue
        if 'PxrMetric:' in line:
            for key,out in [('MTP',mtp),('FrmGpu',gpu),('FrmLate',late)]:
                m=re.search(r'\b'+key+r'=([0-9.]+)',line)
                if m: out.append(float(m.group(1)))
        elif 'render:' in line:
            m=re.search(r'render: (\d+) iterations.*?, (\d+) new-source',line)
            if m: repeats.append(int(m.group(1))-int(m.group(2)))
            m=re.search(r'ready wait attempts (\d+) success (\d+)',line)
            if m: waits.append({'attempts':int(m.group(1)),'successes':int(m.group(2))})
            m=re.search(r'source display-time offset ([0-9.]+) ms',line)
            if m: offsets.append(float(m.group(1)))
    mean=lambda a: round(sum(a)/len(a),3) if a else None
    return {'file':str(path),'warmup_excluded_s':10,'vendor_mtp_not_photon_time':True,
            'pxrmetric':{'n':len(mtp),'mtp_ms_mean':mean(mtp),'frm_gpu_ms_mean':mean(gpu),'frm_late_mean':mean(late)},
            'render':{'n':len(repeats),'repeat_frames_mean':mean(repeats),'source_offset_ms_mean':mean(offsets),'wait_windows':waits}}

for arg in sys.argv[1:]: print(json.dumps(run(arg),separators=(',',':')))
