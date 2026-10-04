from pathlib import Path
import csv,statistics
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent
rows=list(csv.DictReader((p/'scope-alternating.csv').open()))
variants=['baseline','naive','cached'];labels=['Two passes','Repeated conversion','Tile cache']
plt.rcParams.update({'font.size':11,'figure.facecolor':'#f8f7fc','axes.facecolor':'#f8f7fc'})
fig,axs=plt.subplots(1,2,figsize=(11,4.7))
bottom=[0]*3
for m,label,col in [('pass_gpu_us','Colour pass','#7e57c2'),('astc_gpu_us','ASTC','#273955'),('copy_gpu_us','Readback interval','#51a6a6')]:
 vals=[statistics.mean(float(r[m]) for r in rows if r['variant']==v)/1000 for v in variants]
 axs[0].bar(labels,vals,bottom=bottom,label=label,color=col);bottom=[a+b for a,b in zip(bottom,vals)]
axs[0].set_ylabel('Mean GPU interval (ms)');axs[0].set_ylim(0,2.2);axs[0].legend(fontsize=9)
for i,b in enumerate(bottom):axs[0].text(i,b+.025,f'{b:.3f}',ha='center')
axs[1].bar(labels,[48,48,96],color=['#273955','#7e57c2','#51a6a6']);axs[1].set_ylabel('Driver private scratch (KiB / subgroup)');axs[1].set_ylim(0,120)
for i,b in enumerate([48,48,96]):axs[1].text(i,b+2,str(b),ha='center')
for ax in axs:ax.spines[['top','right']].set_visible(False);ax.tick_params(axis='x',labelrotation=12)
fig.suptitle('An exact tile cache removes a pass, but doubles private memory',fontweight='bold')
fig.text(.5,.025,'2176² one eye • RX7900XTX • 12 matched triples • generated identity image\nNo app, transport, Pico or photon latency measurement.',ha='center',fontsize=9)
fig.tight_layout(rect=(0,.10,1,.92));fig.savefig(p/'cache.png',dpi=170)
