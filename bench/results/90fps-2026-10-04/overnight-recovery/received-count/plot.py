from pathlib import Path
import csv,numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent
rows=list(csv.DictReader((p/'host-abba.csv').open()));assert len(rows)==120
plt.style.use('dark_background')
fig,axes=plt.subplots(1,2,figsize=(11,5),sharey=True)
cases=['late_interior_hole','unknown_tail','parity_suppressed']
x=np.arange(3)
for ax,phase,title in zip(axes,['before_due','due'],['Before quiet gate','At quiet gate']):
 for mode,offset,color,label in [('baseline',-.18,'#b7b3c6','Original scans'),('cached',.18,'#aa77ff','Received count')]:
  y=[np.median([float(r['ns_per_call'])/1000 for r in rows if r['mode']==mode and r['phase']==phase and r['scenario']==s]) for s in cases]
  ax.bar(x+offset,y,width=.33,color=color,label=label)
  for a,b in zip(x+offset,y):ax.text(a,b+.035,f'{b:.3f}',ha='center',fontsize=10,color=color)
 ax.set_xticks(x,['Late hole','Unknown tail','Parity hole']);ax.set_ylim(0,2.65);ax.set_title(title);ax.grid(axis='y',alpha=.2)
axes[0].set_ylabel('Median loop-average query cost (microseconds)');axes[0].legend(loc='upper left',fontsize=9)
fig.suptitle('Keep a count; stop rediscovering which slots are filled',fontsize=16)
fig.text(.5,.045,'Host CPU3; ABBA; synthetic six-set / 521-slot occupancy.\nExcludes clocks, locks, sockets, real invocation frequency, Pico GPU and displayed FPS.',ha='center',fontsize=10,color='#d0ccd9')
fig.tight_layout(rect=(0,.13,1,.94));fig.savefig(p/'query-cost.png',dpi=160,facecolor='#100d18')

rows=list(csv.DictReader((p/'pico/abba.csv').open()));assert len(rows)==120
fig,axes=plt.subplots(1,2,figsize=(11,5),sharey=True)
for ax,phase,title in zip(axes,['before_due','due'],['Before quiet gate','At quiet gate']):
 for mode,offset,color,label in [('baseline',-.18,'#b7b3c6','Original scans'),('cached',.18,'#aa77ff','Received count')]:
  y=[np.mean([float(r['ns_per_call'])/1000 for r in rows if r['mode']==mode and r['phase']==phase and r['scenario']==s]) for s in cases]
  ax.bar(x+offset,y,width=.33,color=color,label=label)
  for a,b in zip(x+offset,y):ax.text(a,b+.06,f'{b:.3f}',ha='center',fontsize=10,color=color)
 ax.set_xticks(x,['Late hole','Unknown tail','Parity hole']);ax.set_ylim(0,6.3);ax.set_title(title);ax.grid(axis='y',alpha=.2)
axes[0].set_ylabel('Mean of helper-loop averages (microseconds)');axes[0].legend(loc='upper left',fontsize=9)
fig.suptitle('Same work removed on the Pico CPU',fontsize=16)
fig.text(.5,.045,'Asleep / display OFF / thermal 0 before and after; ABBA; synthetic occupancy.\nNo installed change. Excludes XR, locks, actual call rate, Pico GPU and displayed FPS; clocks/affinity uncontrolled.',ha='center',fontsize=10,color='#d0ccd9')
fig.tight_layout(rect=(0,.13,1,.94));fig.savefig(p/'pico/query-cost.png',dpi=160,facecolor='#100d18')
