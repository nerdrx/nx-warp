from pathlib import Path
import json,csv,numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).parent;m=json.loads((p/'results.json').read_text());names=['pico-first','pico-repeat','pico-shared','pico-final-control'];labels=['Baseline 1','Baseline 2','Shared decisions\n(rejected)','Baseline after']
fig,ax=plt.subplots(1,2,figsize=(11,4.5));x=np.arange(4)
for mode,off,color,label in [('full',-.18,'#8295ac','Fresh full copy'),('guide_history',.18,'#7653cc','Guide + history')]:
 vals=[m[k][mode]['mean_ms'] for k in names];ax[0].bar(x+off,vals,.36,label=label,color=color)
ax[0].set_xticks(x,labels,fontsize=8);ax[0].set(ylabel='GPU dispatch mean (ms)',title='Reconstruction cost, isolated Pico');ax[0].legend(fontsize=8)
for name,color in [('pico-repeat','#7653cc'),('pico-shared','#c66437'),('pico-final-control','#477c6a')]:
 rows=list(csv.DictReader((p/(name+'-timing.csv')).open()));vals=np.sort([float(r['gpu_ms']) for r in rows if r['full']=='0']);ax[1].plot(vals,np.arange(1,len(vals)+1)/len(vals),label=name,color=color)
ax[1].set(xlabel='Guide/history GPU dispatch (ms)',ylabel='Fraction of samples',title='Timing distribution; clocks not locked');ax[1].legend(fontsize=8)
for a in ax:a.grid(axis='y',alpha=.2);a.set_axisbelow(True)
fig.suptitle('Synchronized GPU probe — not codec latency or live FPS',fontsize=13)
fig.tight_layout(rect=[0,.05,1,.94]);fig.text(.5,.01,'uint32 grayscale · static resident inputs · 16 warmups excluded · input/decode/history-cache/presentation costs excluded',ha='center',fontsize=8)
fig.savefig(p/'gpu-cost.png',dpi=150)
