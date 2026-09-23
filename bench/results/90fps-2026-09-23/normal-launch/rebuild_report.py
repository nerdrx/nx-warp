#!/usr/bin/env python3
import csv, json
from pathlib import Path
import matplotlib.pyplot as plt
p=Path(__file__).parent
rows=list(csv.DictReader((p/'windows.csv').open()))
def vals(k): return [(int(r[k]),float(r[k.replace('window','fps')])) for r in rows if r.get(k) and r.get(k.replace('window','fps'))]
fig,ax=plt.subplots(1,2,figsize=(8,3.2),dpi=160)
for key,label in [('server_window','server_fps'),('client_window','render_fps'),('client_window','fresh_source_fps')]:
 x=[int(r[key]) for r in rows if r.get(key) and r.get(label)]; y=[float(r[label]) for r in rows if r.get(key) and r.get(label)]; ax[0].plot(x,y,label=label.replace('_',' '))
x=[int(r['server_window']) for r in rows if r.get('server_window') and r.get('server_payload_mbps')]; y=[float(r['server_payload_mbps']) for r in rows if r.get('server_window') and r.get('server_payload_mbps')]; ax[1].plot(x,y,color='#7700ff')
ax[0].set(xlabel='independent window order',ylabel='frames/s',title='Throughput'); ax[1].set(xlabel='server window order',ylabel='payload Mbps',title='Server payload')
for a in ax:a.grid(alpha=.25);a.tick_params(labelsize=8)
ax[0].legend(fontsize=8);fig.tight_layout();fig.savefig(p/'comparison.png')
json.loads((p/'summary.json').read_text())
