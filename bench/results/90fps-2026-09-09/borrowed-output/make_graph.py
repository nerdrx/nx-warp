#!/usr/bin/env python3
import json
from pathlib import Path
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parent
rows=[json.loads((root/name).read_text()) for name in ('borrowed-copy-last30.json','borrowed-direct-last30.json','borrowed-direct-independent-last30.json')]
labels=['Copy','Direct','Direct +\npublish fix']
fig,axes=plt.subplots(2,2,figsize=(9,6.5),layout='constrained')
for ax,(part,key,title,unit) in zip(axes.flat,[('decoder','copy_gpu_ms','Decoder copy','ms'),('client','own_gpu_ms','Client render GPU','ms'),('client','fresh_per_s','Fresh source updates','updates/s'),('client','source_offset_ms','Source display-time offset','ms; scheduling proxy')]):
 values=[r[part]['means'][key] for r in rows]
 ax.bar(labels,values,color=['#73839a','#4098d7','#35a36b']);ax.set_title(title);ax.set_ylabel(unit);ax.set_ylim(0,max(values)*1.2);ax.grid(axis='y',alpha=.2)
 for i,v in enumerate(values):ax.text(i,v+max(values)*.025,f'{v:.2f}',ha='center')
fig.suptitle('Pico live streaming: means of 30 completed windows\nSame scene; separate connections; smoothing enabled',fontsize=12)
fig.savefig(root/'borrowed-copy-direct-independent.png',dpi=160)
