"""Summarize completed live windows; never treat these as per-frame percentiles."""
import json,subprocess,sys
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parent
parser=root/'analyze_live.py'
rows=[json.loads(subprocess.check_output([sys.executable,str(parser),str(root/n)])) for n in sys.argv[1:]]
(root/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
labels=[Path(r['file']).stem.replace('-client','').replace('-recovered','') for r in rows]
fig,axs=plt.subplots(1,3,figsize=(13,4.7),layout='constrained')
for ax,(key,title) in zip(axs,[('fresh_per_s','Fresh source updates/s'),('own_gpu_ms','Presentation GPU (ms)'),('source_offset_ms','Source display-time offset (ms)')]):
 vals=[r['client']['means'][key] for r in rows]
 bars=ax.bar(labels,vals,color=['#607d8b','#bc8d47','#247ba0','#2a9d8f','#75699c']);ax.bar_label(bars,fmt='%.2f',padding=3);ax.set_ylim(0,max(vals)*1.15);ax.set_title(title,fontsize=11);ax.tick_params(axis='x',rotation=20,labelsize=8);ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
fig.suptitle('Pico fragment-density experiment — last 30 complete window means')
fig.supxlabel('Stationary headless hello_xr. Source offset is not motion-to-photon latency.',fontsize=9)
fig.savefig(root/'live-comparison.png',dpi=160)
print(json.dumps(rows))
