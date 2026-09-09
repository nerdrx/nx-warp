from pathlib import Path
import json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent;data=json.loads((p/'results.json').read_text());fig,axes=plt.subplots(1,2,figsize=(10,4),layout='constrained');labels=[f"{x['run']+1}: "+('Priority' if x['priority'] else 'Control') for x in data];colors=['#21a49a' if x['priority'] else '#6253b5' for x in data]
for ax,key,title in [(axes[0],None,'Encode start → first selection'),(axes[1],'fresh_per_s','Fresh source selections')]:
 if key is None:
  v=[x['latency']['stages']['blit']['p50_p95_p99_ms'] for x in data]
  ax.bar(range(len(data)),[x[0] for x in v],color=colors,label='p50');ax.scatter(range(len(data)),[x[1] for x in v],color='black',marker='x',label='p95');ax.scatter(range(len(data)),[x[2] for x in v],color='#cc6677',marker='_',label='p99');ax.set_ylabel('Milliseconds (lower is better)');ax.legend()
 else:
  ax.bar(range(len(data)),[x['telemetry']['client']['means'][key] for x in data],color=colors);ax.set_ylabel('Selections per second (higher is better)')
 ax.set_xticks(range(len(data)),labels,rotation=15);ax.set_title(title);ax.spines[['right','top']].set_visible(False)
fig.suptitle('Pico: decode queue priority hint — live motion comparison\nSame APK and image settings; four 60-second runs',fontsize=12);fig.savefig(p/'comparison.png',dpi=160)
