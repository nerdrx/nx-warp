from pathlib import Path
import re, json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).parent
summary={}
for tag in ['safety-loss-v3','safety-normal']:
 s=(p/(tag+'.log')).read_text()
 rows=[list(map(float,x)) for x in re.findall(r'render: (\d+) iterations in ([\d.]+) s \(([\d.]+)/s\), (\d+) submitted a layer, (\d+) new-source',s)][2:]
 # Omit the initial partial reporting window and the first startup window.
 elapsed=sum(x[0]/x[2] for x in rows)
 summary[tag]={'fresh_per_s':sum(x[4] for x in rows)/elapsed,'submitted_per_s':sum(x[3] for x in rows)/elapsed,'windows':len(rows),
  'backwards':sum(map(int,re.findall(r'forward \d+ backward (\d+)',s))),
  'switch_ms':[float(x) for x in re.findall(r'NX safety: fallback.*?after ([\d.]+) ms',s) if float(x)>0]}
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
plt.rcParams.update({'font.family':'DejaVu Sans','axes.spines.top':False,'axes.spines.right':False})
fig,ax=plt.subplots(1,2,figsize=(12,4.5),layout='constrained')
values=summary['safety-loss-v3']['switch_ms']
ax[0].bar(range(1,len(values)+1),values,color='#7700ff')
ax[0].axhline(1000/90*2,color='#444444',ls='--',label='Two nominal 90 Hz refreshes')
ax[0].set(title='Fallback takes over after two refreshes',xlabel='Induced detail-loss burst',ylabel='Primary hold before fallback (ms)',ylim=(0,30))
for i,v in enumerate(values,1):ax[0].text(i,v+.6,f'{v:.2f}',ha='center',fontsize=9)
ax[0].legend(loc='lower center',fontsize=8)
for i,tag in enumerate(summary):
 d=summary[tag];ax[1].bar(i-.16,d['submitted_per_s'],width=.3,color='#ccccdd',label='Submitted layers' if i==0 else None)
 ax[1].bar(i+.16,d['fresh_per_s'],width=.3,color='#7700ff',label='Fresh source selections' if i==0 else None)
 ax[1].text(i+.16,d['fresh_per_s']+1,f"{d['fresh_per_s']:.1f}",ha='center')
ax[1].set(xticks=[0,1],xticklabels=['30 / 180 detail frames lost','No induced loss'],ylim=(0,100),ylabel='Per second',title='Picture updates continue during loss')
ax[1].legend(loc='lower center',fontsize=8)
fig.suptitle('NX safety prefix · Pico · 2176² per eye · 160 Mbit/s setting',fontsize=15,fontweight='bold')
fig.supxlabel('Short synthetic trials; software submission evidence. Not photon latency or a visual comfort test.',fontsize=9)
fig.savefig(p/'handover.png',dpi=160)
print(json.dumps(summary,indent=2))
