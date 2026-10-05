import csv,gzip
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
r=Path(__file__).resolve().parent
plt.rcParams.update({"font.size":10,"axes.spines.top":False,"axes.spines.right":False,"svg.fonttype":"none"})
colors={"D":"#7443b6","G":"#148980"}
def load(path):
 with gzip.open(path,"rt") as f:return list(csv.DictReader(f))
fig,axes=plt.subplots(2,2,figsize=(12.4,8))
for ax,policy in zip(axes[0],("service","paced")):
 for mode in ("D","G"):
  rows=[x for x in load(r/f'raw/{mode}-results/noisy-{policy}.csv.gz') if x['scenario']=='moderate-150' and x['phase']=='360']
  ax.plot([int(x['elapsed_ns'])/1e9 for x in rows],[int(x['bitrate_bps'])/1e6 for x in rows],label=mode,color=colors[mode],ls='-' if mode=='D' else '--',lw=1.8)
 ax.axhline(24,color='#777777',ls=':',label='Assumed link: 24 Mbps')
 ax.set(xlabel='Virtual elapsed time (s)',ylabel='Controller target (Mbps)',ylim=(14,42),title='A. Sender spans include service' if policy=='service' else 'B. Sender spans include pacing only')
 ax.legend(loc='upper right');ax.grid(alpha=.18)
ax=axes[1,0]
for mode in ("D","G"):
 rows=load(r/f'raw/{mode}-results/capacity/rise20-clean.csv.gz')
 ax.plot([int(x['start_ns'])/1e9 for x in rows],[int(x['bitrate_bps'])/1e6 for x in rows],label=mode,color=colors[mode],ls='-' if mode=='D' else '--',lw=1.8)
ax.axvline(20,color='#777777',ls=':');ax.axhline(840,color='#777777',ls=':')
ax.set(xlim=(18,26),ylim=(250,1040),xlabel='Virtual sender start (s)',ylabel='Controller target (Mbps)',title='C. Clean 20 s capacity rise: byte-identical traces')
ax.text(20.9,650,'First target ≥840 Mbps:\n20.833333125 s, both modes',fontsize=10)
ax.legend();ax.grid(alpha=.18)
ax=axes[1,1]
cases=['one-fast-receive','serial-send-overlap-receive','missing-one-send','quality-path-unchanged'];labels=['One stream,\nreceive compressed','Serial sends,\noverlap receives','Missing one\nsend interval','Direct quality\nexempt']
for n,mode in enumerate(('D','G')):
 data={x['case']:int(x['rate_bps'])/1e6 for x in load(r/f'raw/{mode}-results/span_cases.csv.gz')}
 ax.bar(np.arange(4)+(n-.5)*.34,[data[x] for x in cases],width=.34,color=colors[mode],label=mode)
ax.set_xticks(range(4),labels);ax.set(ylabel='Reported rate (Mbps)',title='D. Independent matched-interval boundary cases');ax.set_ylim(0,640);ax.legend();ax.grid(axis='y',alpha=.18)
fig.suptitle('Matched delivery spans: conditional benefit, preserved clean recovery',fontsize=15)
fig.tight_layout(rect=(0,.05,1,.94),h_pad=2)
fig.text(.5,.018,'Actual controller on virtual clocks. Synthetic timing regimes; no live FPS, latency or link-capacity proof.',ha='center',fontsize=10,color='#555555')
for suffix in ('png','svg'):fig.savefig(r/f'matched-spans.{suffix}',dpi=160)
p=r/'matched-spans.svg';p.write_text('\n'.join(x.rstrip() for x in p.read_text().splitlines())+'\n')
