from pathlib import Path
import json
import matplotlib.pyplot as plt
p=Path(__file__).parent
d=json.loads((p/'summary.json').read_text())
fig,ax=plt.subplots(figsize=(7,4),layout='constrained')
b=ax.bar(['500 Mbit/s','200 Mbit/s'],[d['500']['mean_fresh_fps'],d['200']['mean_fresh_fps']],color=['#7700ff','#00a6c7'])
ax.bar_label(b,fmt='%.1f',padding=4)
ax.axhline(90,color='#555',linestyle='--',label='90 Hz target')
ax.set_ylim(0,105);ax.set_ylabel('Fresh source frames per second')
ax.set_title('Live Pico: packet CPU improved; 500 still fails')
ax.legend();fig.savefig(p/'fresh-frames.png',dpi=160)
