#!/usr/bin/env python3
import csv
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent
rows=list(csv.DictReader((p/'outcomes.csv').open()))
names=['quiet','deadline_off','timely_direct_repair']
labels=['Quiet timeout + late arrival','Retirement disabled','Timely direct repair']
fig,ax=plt.subplots(figsize=(10.2,4.4))
for mode,delta,color in [('normal',-.16,'#734be8'),('san',.16,'#17a398')]:
 vals=[int(next(r for r in rows if r['mode']==mode and r['case']==n)['decoder_stub_completions']) for n in names]
 ax.barh([i+delta for i in range(3)],vals,height=.28,label=mode,color=color)
ax.set_yticks(range(3),labels);ax.invert_yaxis();ax.set_xticks([0,1,2]);ax.set_xlim(0,2.3)
ax.set_xlabel('Frames completed by decoder substitute (count)')
ax.set_title('Host UDP arrival gate: observed completions',loc='left',weight='bold')
ax.legend(loc='lower right');ax.grid(axis='x',alpha=.2);ax.set_axisbelow(True)
fig.text(.03,.025,'29 assertions per mode. Real typed localhost arrivals; scene, decoder, XR and config substitutes. No FPS or latency claim.',fontsize=8.5)
fig.tight_layout(rect=[0,.06,1,1]);fig.savefig(p/'outcomes.png',dpi=170);fig.savefig(p/'outcomes.svg');plt.close(fig)
