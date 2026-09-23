import json
from pathlib import Path
import matplotlib.pyplot as plt
p=Path('/run/media/nerdrx/Lex/claude/nx-warp/bench/results/90fps-2026-09-23/wire-admission'); d=json.load(open(p/'results.json'))
labels=['raw admission','wire admission']; colors=['#777777','#7700ff'];
fig,ax=plt.subplots(1,2,figsize=(10,4.5),dpi=160)
metrics=[('encoder_fps','encoder FPS'),('viewer_new_source_fps','viewer new-source FPS'),('app_renderloop_fps','app render-loop FPS')]
import numpy as np
x=np.arange(len(metrics)); width=.34
for j,k in enumerate(('raw','wire')):
 bars=ax[0].bar(x+(j-.5)*width,[d[k]['summary'][m[0]] for m in metrics],width,label=labels[j],color=colors[j]); ax[0].bar_label(bars,fmt='%.1f',padding=3,fontsize=8)
ax[0].set_xticks(x, [m[1].replace(' FPS','') for m in metrics],rotation=18,ha='right'); ax[0].set_ylabel('frames/s'); ax[0].set_ylim(0,118); ax[0].legend(loc='upper left',ncol=2,fontsize=8); ax[0].grid(axis='y',alpha=.25)
for j,k in enumerate(('raw','wire')):
 bars=ax[1].bar(j,d[k]['summary']['payload_mbps'],color=colors[j],label=labels[j]); ax[1].bar_label(bars,fmt='%.2f',padding=3)
ax[1].set_xticks([0,1],labels); ax[1].set_ylabel('measured Zstd payload Mbps'); ax[1].set_ylim(0,max(d['raw']['summary']['payload_mbps'],d['wire']['summary']['payload_mbps'])*1.2); ax[1].grid(axis='y',alpha=.25)
fig.suptitle('Fixed25 A/B: raw admission vs wire admission\nMiddle windows only; no photon/display-latency claim',fontsize=11); fig.tight_layout(); fig.savefig(p/'comparison.png');
