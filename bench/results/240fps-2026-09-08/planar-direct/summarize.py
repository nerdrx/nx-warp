#!/usr/bin/env python3
import csv, json, math, re, tarfile
from pathlib import Path
ROOT=Path(__file__).parent
def nr(v,q):
    a=sorted(v); return a[max(0,min(len(a)-1,int(math.ceil(q*len(a)/100))-1))] if a else None
def main():
    rows={}
    with tarfile.open(ROOT/'raw-captures.tar.gz','r:gz') as t:
      for m in t.getmembers():
        if not m.name.endswith('.csv'): continue
        data=t.extractfile(m).read().decode(errors='replace').splitlines(); rd=list(csv.DictReader(data))
        if not rd: continue
        stats={}
        for k in ('gpu_ms','total_ms','interval_ms','arrival_late_ms'):
          vals=[]
          for r in rd:
            if k in r and r[k]:
              x=float(r[k]);
              if not math.isfinite(x) or x<0: raise ValueError(f'{m.name}: bad {k}')
              vals.append(x)
          if vals:
            warm=[float(r[k]) for r in rd[24:] if r.get(k)]
            stats[k]={'all':{'count':len(vals),'median_ms':nr(vals,50),'p95_ms':nr(vals,95),'p99_ms':nr(vals,99),'max_ms':max(vals)},
                      'warmup_excluded':{'count':len(warm),'median_ms':nr(warm,50),'p95_ms':nr(warm,95),'p99_ms':nr(warm,99),'max_ms':max(warm) if warm else None}}
        total=[float(r['total_ms']) for r in rd if r.get('total_ms')]
        cadence=[float(r['interval_ms']) for r in rd if r.get('interval_ms')]
        total_warm=[float(r['total_ms']) for r in rd[24:] if r.get('total_ms')]
        rows[Path(m.name).name]={'frames':len(rd),'stats':stats,
          'deadline_misses':sum(x>4.167 for x in total),
          'deadline_miss_pct':100*sum(x>4.167 for x in total)/len(total) if total else None,
          'warmup_excluded_deadline_misses':sum(x>4.167 for x in total_warm),
          'warmup_excluded_deadline_miss_pct':100*sum(x>4.167 for x in total_warm)/len(total_warm) if total_warm else None,
          'cadence_over_4_167_ms':sum(x>4.167 for x in cadence)}
    inclusive=None
    with tarfile.open(ROOT/'raw-captures.tar.gz','r:gz') as t:
      for m in t.getmembers():
        if m.name.endswith('paced-7200.log'):
          text=t.extractfile(m).read().decode(errors='replace')
          q=re.search(r'inclusive_fps=([0-9.]+)',text)
          if q: inclusive=float(q.group(1))
    out={'fixture':'4352x2176 synthetic all-PLANAR changing-pixel pan','runs':rows,
         'paced_7200_inclusive_fps':inclusive,'deadline_ms':4.167,'warmup_excluded_frames':24}
    (ROOT/'summary.json').write_text(json.dumps(out,indent=2)+'\n')
    paced=rows.get('paced-7200.csv')
    if paced:
      names=['gpu_ms','interval_ms']; vals=[paced['stats'].get(k,{}).get('warmup_excluded',{}).get('median_ms') for k in names]
    else: vals=[]
    try:
      import matplotlib.pyplot as plt
      if vals:
        fig,ax=plt.subplots(figsize=(5.2,3.2),dpi=140); ax.bar(['GPU','completion interval'],vals,color=['#4c78a8','#f58518']); ax.axhline(4.167,color='#d62728',ls='--',lw=1); ax.set_ylabel('Warmup-excluded median (ms)'); ax.set_title('paced-7200'); fig.tight_layout(); fig.savefig(ROOT/'paced7200-summary.png'); plt.close(fig)
      with tarfile.open(ROOT/'raw-captures.tar.gz','r:gz') as t:
        member=next((m for m in t.getmembers() if m.name.endswith('paced-7200.csv')),None)
        if member:
          rd=list(csv.DictReader(t.extractfile(member).read().decode(errors='replace').splitlines()))
          fig,ax=plt.subplots(figsize=(8,3.2),dpi=140); ax.plot([float(r['interval_ms']) for r in rd],lw=.6,color='#4c78a8'); ax.axhline(4.167,color='#d62728',ls='--',lw=1,label='240 FPS deadline'); ax.set(xlabel='Frame',ylabel='Completion interval (ms)'); ax.legend(); fig.tight_layout(); fig.savefig(ROOT/'paced7200-trace.png'); plt.close(fig)
    except ImportError:
      pass
if __name__=='__main__': main()
