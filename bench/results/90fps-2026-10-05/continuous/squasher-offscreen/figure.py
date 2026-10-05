#!/usr/bin/env python3
import csv
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import ListedColormap
p=Path(__file__).resolve().parent
rows=list(csv.DictReader((p/'samples.csv').open()))
v=np.array([[float(r[c]) for c in ['R','G','B','A']] for r in rows])
fig,ax=plt.subplots(figsize=(9,6))
ax.imshow(v,vmin=0,vmax=1,cmap=ListedColormap(['#f0eef6','#7953d9']),aspect='auto')
ax.set_xticks(range(4),['R','G','B','A'])
ax.set_yticks(range(10),[f"Eye {r['eye']} · sample {r['sample']}" for r in rows])
for y in range(10):
 for x in range(4):ax.text(x,y,f'{v[y,x]:g}',ha='center',va='center',color='white' if v[y,x]>.5 else '#302848')
ax.axhline(4.5,color='#302848',lw=2)
ax.set_title('Production stereo squasher: observed GPU readback\n2176×2176 per eye · RX 7900 XTX',pad=14)
fig.text(.5,.025,'40 finite RGBA checks pass. Solid fixtures; five locations per eye. No FPS/latency measurement.',ha='center',fontsize=10)
fig.tight_layout(rect=[0,.06,1,1])
fig.savefig(p/'samples.png',dpi=180);fig.savefig(p/'samples.svg');plt.close(fig)
