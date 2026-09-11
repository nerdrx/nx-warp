from pathlib import Path
import json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
r=Path('/run/media/nerdrx/Lex/claude/nx-warp/bench/results/90fps-2026-09-11/hevc-60-warp')
rows=json.loads((r/'raw/summary.json').read_text())
labels=['Admission off A','Admission on','Admission off B','Pacing off A','Pacing on','Pacing off B']
fig,ax=plt.subplots(1,2,figsize=(11,4.5),layout='constrained')
colors=['#aaa','#b872db','#aaa','#aaa','#663399','#aaa']
for a,key,title in zip(ax,['fresh_per_s','own_gpu_ms'],['Fresh source selections / second','Presentation GPU time (ms)']):
 a.bar(range(len(rows)),[x['client']['means'][key] for x in rows],color=colors)
 a.set_xticks(range(len(rows)),labels,rotation=35,ha='right');a.set_title(title);a.grid(axis='y',alpha=.2);a.set_axisbelow(True)
ax[0].axhline(60,ls='--',color='black',lw=1,label='60 source target');ax[0].legend()
fig.suptitle('Pico 2688²/eye · HEVC 10-bit · 30-second screens\nShort-window means; stationary headset; not physical latency')
fig.savefig(r/'comparison.png',dpi=160)
