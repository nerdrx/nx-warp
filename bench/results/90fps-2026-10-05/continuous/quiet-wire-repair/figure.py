#!/usr/bin/env python3
import csv
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent
rows=list(csv.DictReader((p/'outcomes.csv').open()));normal=[r for r in rows if r['mode']=='normal']
assert len(normal)==3
for r in normal:
 other=next(x for x in rows if x['mode']=='san' and x['case']==r['case']);assert all(r[k]==other[k] for k in r if k!='mode')
labels=['Returned repair','Reply dropped','Enabled history miss'];x=range(3)
fig,axes=plt.subplots(1,2,figsize=(11,4.6))
completed=[int(r['completed']) for r in normal];retired=[int(r['retired']) for r in normal]
axes[0].bar(x,completed,color='#7654dc',label='Completed by substitute');axes[0].bar(x,retired,bottom=completed,color='#e4ab48',label='Retired incomplete')
axes[0].set_title('Frame outcomes');axes[0].set_ylim(0,2.8);axes[0].set_yticks([0,1,2]);axes[0].legend(fontsize=8,loc='upper right')
for field,offset,color in [('nacks',-.24,'#7654dc'),('hits',0,'#2cae9b'),('replies',.24,'#e4ab48')]:axes[1].bar([i+offset for i in x],[int(r[field]) for r in normal],width=.22,label=field,color=color)
axes[1].set_title('Requests and retained replies');axes[1].set_ylim(0,2.8);axes[1].set_yticks([0,1,2]);axes[1].legend(fontsize=8,loc='upper right')
for ax in axes:ax.set_xticks(list(x),labels,rotation=12);ax.set_ylabel('Count');ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
fig.suptitle('Host wire-repair join: observed functional outcomes',weight='bold')
fig.text(.03,.02,'Normal and ASan/UBSan agree; 80 assertions each. Synchronous localhost peer, synthetic payloads and decoder substitute. No FPS/latency claim.',fontsize=8.5)
fig.tight_layout(rect=[0,.07,1,.93]);fig.savefig(p/'outcomes.png',dpi=170);fig.savefig(p/'outcomes.svg');plt.close(fig)
