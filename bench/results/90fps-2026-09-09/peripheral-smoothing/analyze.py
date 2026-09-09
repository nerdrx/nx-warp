#!/usr/bin/env python3
"""Summarize the last completed two-second render windows in the captures."""
import re, pathlib, statistics
import matplotlib.pyplot as plt
import csv
ROOT = pathlib.Path(__file__).resolve().parent
PAT = {
 'window': re.compile(r'render: (\d+) iterations.*?(\d+) new-source'),
 'gpu': re.compile(r'own GPU pass ([0-9.]+) ms'),
 'offset': re.compile(r'source display-time offset ([0-9.-]+) ms'),
 'lead': re.compile(r'submit lead ([0-9.-]+) ms'),
}
def rows(path):
    lines = path.read_text(errors='replace').splitlines()
    out=[]; cur={}
    for line in lines:
        m=PAT['window'].search(line)
        if m: cur={'iterations':int(m.group(1)), 'fresh':int(m.group(2))}
        for k in ('gpu','offset','lead'):
            m=PAT[k].search(line)
            if m:
                cur[k]=float(m.group(1))
                if k=='lead' and len(cur)==5: out.append(cur); cur={}
    return out[-15:]
def avg(rs,k): return statistics.fmean(r[k] for r in rs)
sets=[('smoothing on','two-tap-matched-on.log'),('smoothing off','two-tap-matched-off.log'),('pacing .4 baseline','baseline-last15-windows.log'),('pacing .1 client','lowlat-last15-windows.log')]
summary=[]
for name,fn in sets:
    p=ROOT/fn
    if not p.exists(): continue
    rs=rows(p) if 'matched' in fn else []
    if rs: summary.append((name,avg(rs,'gpu'),avg(rs,'fresh')/2.0,avg(rs,'offset'),avg(rs,'lead')))
    elif 'baseline' in fn or 'lowlat' in fn:
        lines=p.read_text().splitlines(); offsets=[]; leads=[]
        for line in lines:
            m=PAT['offset'].search(line)
            if m: offsets.append(float(m.group(1)))
            m=PAT['lead'].search(line)
            if m: leads.append(float(m.group(1)))
        summary.append((name,None,None,statistics.fmean(offsets),statistics.fmean(leads)))
print('condition,gpu_ms,fresh_per_s,source_offset_ms,submit_lead_ms')
with (ROOT/'metrics.csv').open('w', newline='') as f:
    w=csv.writer(f); w.writerow(['condition','gpu_ms','fresh_per_s','source_offset_ms','submit_lead_ms'])
    for row in summary:
        print(','.join('' if x is None else f'{x:.3f}' if isinstance(x,float) else x for x in row))
        w.writerow(row)
labels=[r[0] for r in summary if r[1] is not None]; gpu=[r[1] for r in summary if r[1] is not None]; fresh=[r[2] for r in summary if r[1] is not None]
fig, (ax, ax2)=plt.subplots(1,2,figsize=(8,3.6)); x=range(len(labels))
ax.bar(list(x),gpu,color='#e07a5f'); ax2.bar(list(x),fresh,color='#4c9aff')
for a, vals, ylabel in ((ax,gpu,'GPU pass (ms)'),(ax2,fresh,'fresh updates/s')):
    a.set_xticks(list(x),labels,rotation=24,ha='right'); a.set_ylabel(ylabel); a.grid(axis='y',alpha=.25)
fig.suptitle('Pico capture: last 15 completed windows'); fig.tight_layout(); fig.savefig(ROOT/'matched-cost-fresh.png',dpi=150)
