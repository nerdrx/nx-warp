from pathlib import Path
import json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).parent
s=json.loads((p/'summary.json').read_text())['runs']
c=[json.loads(x) for x in (p/'coverage.jsonl').read_text().splitlines()]
fig,axes=plt.subplots(1,2,figsize=(10,4),layout='constrained')
labels=['Priority 1 A','Priority 0 A','Priority 0 B','Priority 1 B']
for ax,values,title,ylabel in [(axes[0],[r['client']['means']['source_offset_ms'] for r in s],'Source-time offset (proxy)','milliseconds'),(axes[1],[r['fresh_per_covered_wall_second'] for r in c],'Fresh source selections','selections / covered wall-second')]:
 ax.plot(labels,values,'o-',color='#7057d9');ax.set_title(title);ax.set_ylabel(ylabel);ax.grid(axis='y',alpha=.25)
 for i,v in enumerate(values):ax.annotate(f'{v:.2f}',(i,v),xytext=(0,9),textcoords='offset points',ha='center')
 ax.margins(y=.3)
fig.suptitle('Pico: decode queue priority at 2688 per eye — four 60-second moving-scene trials')
fig.savefig(p/'per-run.png',dpi=160)
