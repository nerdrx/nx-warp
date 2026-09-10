from pathlib import Path
import json,re,sys
p=Path(__file__).parent;rows=[];proxy=None
pat=re.compile(r'source->first ([-\d.]+) ms \| wire ([-\d.]+) \| queue ([-\d.]+) \| decode ([-\d.]+) \| decode->selection ([-\d.]+) \| selection->predicted ([-\d.]+) \| sum ([-\d.]+) over (\d+) selected frames \((\d+) invalid\)')
for line in Path(sys.argv[1]).read_text().splitlines():
 m=re.search(r'source display-time offset ([-\d.]+) ms',line)
 if m:proxy=float(m[1])
 m=pat.search(line)
 if m:
  x=list(map(float,m.groups()[:7]));n=int(m[8]);invalid=int(m[9]);rows.append(dict(stages=x[:6],sum_ms=x[6],n=n,invalid=invalid,proxy_ms=proxy))
rows=rows[5:];assert rows and all(r['n']>0 for r in rows)
for r in rows:
 assert abs(sum(r['stages'])-r['sum_ms'])<.36
 if not r['invalid']:assert abs(r['sum_ms']-r['proxy_ms'])<.11
n=sum(r['n'] for r in rows);means=[sum(r['stages'][i]*r['n'] for r in rows)/n for i in range(6)]
d=dict(excluded_initial_windows=5,windows=len(rows),selected_samples=n,invalid=sum(r['invalid'] for r in rows),means_ms=means,total_ms=sum(r['sum_ms']*r['n'] for r in rows)/n,rows=rows)
(p/'summary.json').write_text(json.dumps(d,indent=2)+'\n');print(json.dumps({k:v for k,v in d.items() if k!='rows'},indent=2))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
fig,ax=plt.subplots(figsize=(10,4),layout='constrained');labels=['Source stamp\nto first packet','Packet span','Decoder queue','Decode wall','Ready to\nselection','Selection to\npredicted refresh'];ax.bar(labels,means,color=['#718096','#7ca6ba','#7ca6ba','#2583b5','#2583b5','#bd9445']);ax.axhline(0,color='black',linewidth=.6);ax.set(ylabel='Selected-frame weighted mean (ms)',title='2688² per eye: source-offset proxy budget (not photon latency)');ax.grid(axis='y',alpha=.15);ax.set_axisbelow(True);fig.savefig(p/'budget.png',dpi=150)
