#!/usr/bin/env python3
"""Plot recorded window means, not frame percentiles or photon latency."""
import json,subprocess,sys
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parent
names=['live-native-a-client.log','live-compact-a-recovered.log','live-native-b-client.log','live-compact-branchless-client.log','live-native-final-client.log','live-compact-bilinear-client.log','live-compact-bilinear-repeat-client.log']
rows=[json.loads(subprocess.check_output([sys.executable,str(root/'analyze_live.py'),str(root/'logs'/n)])) for n in names]
(root/'live-summary.json').write_text(json.dumps(rows,indent=2)+'\n')
labels=['Native A','Compact\nbranched','Native B','Compact\nbranchless','Native C','Compact\nbilinear only','Bilinear\nrepeat']
fig,axs=plt.subplots(1,3,figsize=(18,5),layout='constrained')
for ax,(title,section,key,unit) in zip(axs,[('Fresh source updates','client','fresh_per_s','updates/s'),('Application presentation GPU','client','own_gpu_ms','ms'),('Source display-time offset','client','source_offset_ms','ms')]):
 vals=[r[section]['means'][key] for r in rows]
 bars=ax.bar(labels,vals,color=['#457b9d','#cb7e32','#457b9d','#2a9d8f','#457b9d','#8f64a9','#a182b5'])
 ax.bar_label(bars,fmt='%.2f',padding=3);ax.set_ylim(0,max(vals)*1.18);ax.set_title(title,fontsize=11);ax.set_ylabel(unit);ax.tick_params(axis='x',labelsize=9);ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
fig.suptitle('Pico live streaming — last 30 complete timing windows per run')
fig.supxlabel('Stationary hello_xr; dark-environment overlay present. Source offset is not motion-to-photon latency.',fontsize=9)
fig.savefig(root/'live-comparison.png',dpi=150)
