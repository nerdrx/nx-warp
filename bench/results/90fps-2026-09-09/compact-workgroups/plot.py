from pathlib import Path
import json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
p=Path(__file__).resolve().parent
rows=json.loads((p/'results.json').read_text())['runs']
fig,ax=plt.subplots(figsize=(7,4.5))
for x,wg in enumerate([256,128,64]):
 r=[a for a in rows if a['wg']==wg]
 a=np.mean([t['passA_mean_ms'] for t in r]); b=np.mean([t['passB_mean_ms'] for t in r])
 ax.bar(x,a,color='#4775ad',label='Pass A' if x==0 else None)
 ax.bar(x,b,bottom=a,color='#e29a49',label='Pass B' if x==0 else None)
 ax.scatter([x]*len(r),[t['gpu_mean_ms'] for t in r],color='black',s=18,zorder=3)
 ax.text(x,a+b+.3,f"{np.mean([t['gpu_mean_ms'] for t in r]):.3f} ms",ha='center')
ax.set_xticks([0,1,2],['256 (control)','128','64'])
ax.set_ylabel('Mean decoder GPU time (ms)')
ax.set_xlabel('Threads per compact flat workgroup')
ax.set_ylim(0,23);ax.legend(loc='upper center',ncol=2);ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
ax.set_title('Pico compact decode: smaller groups barely help')
fig.subplots_adjust(bottom=.23,top=.88)
fig.text(.5,.035,'Standalone moving-patch fixture; unlocked clocks. Dots = individual run means.\nNot live frame latency. All decoded outputs match byte-for-byte.',ha='center',fontsize=8)
fig.savefig(p/'workgroups.png',dpi=160)
