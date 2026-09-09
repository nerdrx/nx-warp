from pathlib import Path
import json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent;r=json.loads((p/'results.json').read_text());vals=list(r['stages'].values());labels=['First → last packet','Last packet → worker','Worker → completion','Completion → selection'];colors=['#4477aa','#66ccee','#aa3377','#228833']
fig,axes=plt.subplots(2,1,figsize=(10,6),layout='constrained',gridspec_kw={'height_ratios':[1,2]});left=0
for v,l,c in zip(vals,labels,colors):
 axes[0].barh([0],v['mean_ms'],left=left,label=l,color=c);axes[0].text(left+v['mean_ms']/2,0,f"{v['mean_ms']:.2f}",ha='center',va='center',color='white',fontweight='bold');left+=v['mean_ms']
axes[0].set_yticks([]);axes[0].set_xlim(0,23);axes[0].set_xlabel('Mean milliseconds after first packet — additive common cohort');axes[0].set_title(f'First packet → first selection: {left:.2f} ms mean')
for j,q in enumerate(['p50','p95','p99']):
 axes[1].barh([i+(j-1)*.23 for i in range(4)],[v['p50_p95_p99_ms'][j] for v in vals],.22,label=q,color=['#4477aa','#66ccee','#aa3377'][j])
axes[1].set_yticks(range(4),labels);axes[1].invert_yaxis();axes[1].set_xlabel('Milliseconds per interval — percentiles are not additive');axes[1].legend(loc='lower right')
for ax in axes:ax.spines[['right','top']].set_visible(False)
fig.suptitle('Live Pico baseline: where selected frames spend time\n6,456 matched frames; existing 90-second animated-scene capture',fontsize=13);fig.savefig(p/'stages.png',dpi=160)
