import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).parent;r={x['tag']:x for x in json.loads((p/'summary.json').read_text())}
names=['lz4-off160','lz4-cached160','lz4-off500','lz4-final500'];labels=['160 raw','160 LZ4','500 raw','500 LZ4'];colors=['#888888','#7700ff','#888888','#7700ff']
fig,ax=plt.subplots(1,2,figsize=(11,4));fig.suptitle('Native NX + LZ4 · short live Pico comparisons')
for a,key,title in [(ax[0],'fresh_after_first','Fresh source updates / second'),(ax[1],'receive_to_predicted_ms','Receive → predicted display (ms)')]:
 vals=[r[n][key] for n in names];a.bar(labels,vals,color=colors);a.set_title(title);a.set_ylim(0,max(vals)*1.2)
 for i,v in enumerate(vals):a.text(i,v+max(vals)*.02,f'{v:.1f}',ha='center')
ax[0].axhline(90,color='#00a9a9',ls='--',lw=1)
fig.text(.5,.01,'160 / 500 denote encoder settings, not measured link rate. Software timing, not optical latency.',ha='center',fontsize=9)
fig.tight_layout(rect=[0,.045,1,.94]);fig.savefig(p/'live-results.png',dpi=160)
