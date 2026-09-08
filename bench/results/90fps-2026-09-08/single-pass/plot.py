import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root = Path(__file__).parent
r = json.loads((root / 'summary.json').read_text())
fig, ax = plt.subplots(figsize=(9,4), constrained_layout=True)
for i,(name,x) in enumerate(r.items()):
    t = x['total_ms']
    ax.bar(i-.17,t['p50_ms'],width=.34,color='#288bb5',label='Median' if i==0 else None)
    ax.bar(i+.17,t['p99_ms'],width=.34,color='#de9848',label='p99' if i==0 else None)
ax.axhline(1000/90,color='red',linestyle='--',label='90 Hz deadline')
ax.set_xticks(range(len(r)),r.keys())
ax.set_ylabel('Scheduled arrival to completion (ms)')
ax.set_title('Pico 4 · native stereo camera motion · 720 frames per arm')
ax.legend(ncol=3)
fig.savefig(root/'comparison.png',dpi=150)
