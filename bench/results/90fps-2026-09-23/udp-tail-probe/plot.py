import json
from pathlib import Path
import matplotlib.pyplot as plt
p=Path(__file__).parent; d=json.load(open(p/'summary.json'))
names=['confirm0_count3_delay0','confirm2_count64_delay1','confirm3_count64_delay500','confirm5_count64_delay1','tailbytes16_count64_delay500','tailbytes16_count64_delay1000','qos184_tail0','pace0_tail32']
labels=['base\n0','tail 3\n1µs','tail 64\n500µs','tail 64\n1µs','16B×64\n500µs','16B×64\n1ms','EF QoS\nreject','1200B×32']
vals=[d[x]['kernel_first_last_ms_median'] for x in names]; p99=[d[x]['kernel_first_last_ms_p99'] for x in names]; loss=[d[x]['lost_datagrams'] for x in names]
fig,ax=plt.subplots(1,2,figsize=(14,5.6),dpi=160); x=range(len(names)); colors=['#777']*4+['#7700ff']*2+['#28a','#b55']; bars=ax[0].bar(x,vals,color=colors); ax[0].set_xticks(list(x),labels,rotation=38,ha='right'); ax[0].set_ylabel('kernel first-last span (ms)'); ax[0].set_title('Burst span median; p99 labels'); ax[0].grid(axis='y',alpha=.25)
for b,v,q in zip(bars,vals,p99): ax[0].text(b.get_x()+b.get_width()/2,b.get_height()+.28,f'{v:.2f}\np99 {q:.2f}',ha='center',fontsize=7)
ax[1].bar(x,loss,color=colors); ax[1].set_xticks(list(x),labels,rotation=38,ha='right'); ax[1].set_ylabel('lost datagrams (full run)'); ax[1].set_title('Unique-sequence loss'); ax[1].grid(axis='y',alpha=.25); fig.suptitle('Standalone UDP burst probe — no one-way/photon claim'); fig.tight_layout(rect=(0,0,1,.94)); fig.savefig(p/'comparison.png')
