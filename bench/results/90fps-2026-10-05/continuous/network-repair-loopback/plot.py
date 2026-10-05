#!/usr/bin/env python3
"""Plot one retained normal loopback trace; never aggregate sanitizer timings."""
import csv
import pathlib
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

here=pathlib.Path(__file__).resolve().parent
rows=[]
with (here/'raw/luna-normal.log').open() as f:
    for row in csv.DictReader(f):
        if row.get('frame')=='40' and row.get('mono_ns','').isdigit():
            rows.append(row)
assert rows and all(int(b['mono_ns'])>=int(a['mono_ns']) for a,b in zip(rows,rows[1:]))
origin=int(rows[0]['mono_ns'])
elapsed=[(int(row['mono_ns'])-origin)/1e6 for row in rows]
labels=[]
for row in rows:
    name=row['event'].replace('_',' ')
    if row['event']=='client_shard': name+=f" {row['index']}"
    labels.append(name)
fig,ax=plt.subplots(figsize=(10,6.5),layout='constrained')
fig.get_layout_engine().set(rect=(0,.055,1,.945))
fig.suptitle('NXVC repair request → exact history reply → reassembly',fontsize=15,weight='bold')
ys=list(range(len(rows)))
ax.hlines(ys,0,elapsed,color='#d2cbe7',lw=2)
ax.scatter(elapsed,ys,color='#7345bd',s=55,zorder=3)
for x,y in zip(elapsed,ys):
    ax.annotate(f'{x:.3f} ms',(x,y),xytext=(8,0),textcoords='offset points',va='center',fontsize=9)
ax.set_yticks(ys,labels)
ax.invert_yaxis()
ax.set_xlim(-max(elapsed)*.02,max(elapsed)*1.27)
ax.set_xlabel('Host elapsed since first client shard callback (ms)')
ax.set_title('One functional IPv6 loopback run; projected scene/session and real typed sockets',fontsize=10)
ax.grid(axis='x',alpha=.25)
ax.spines[['top','right']].set_visible(False)
fig.text(.5,.005,'Reassembly completion only. Not display latency, RF recovery, fresh FPS or a performance comparison.',ha='center',fontsize=9)
fig.savefig(here/'repair-timeline.png',dpi=160,bbox_inches='tight')
fig.savefig(here/'repair-timeline.svg',bbox_inches='tight')
