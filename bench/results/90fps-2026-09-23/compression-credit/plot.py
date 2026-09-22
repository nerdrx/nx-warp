from pathlib import Path
import json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p=Path(__file__).parent
rows=json.loads((p/"wire-sizes.json").read_text());x=np.arange(len(rows))
fig,ax=plt.subplots(figsize=(9,4.2),layout="constrained")
for shift,key,label,color in [(-.18,"reference500_lz4_bytes","Old 500 target + LZ4","#888888"),(.18,"credit350_zstd_bytes","350 target + Zstd + quality credit","#7700ff")]:
 ax.bar(x+shift,[r[key]/1000 for r in rows],.36,label=label,color=color)
for i,r in enumerate(rows):ax.text(i+.18,r["credit350_zstd_bytes"]/1000+3,f"−{r['saving_percent']:.1f}%",ha="center")
ax.set_xticks(x,[r["scene"].capitalize() for r in rows]);ax.set_ylabel("Safety + detail payload (kB/frame)");ax.set_title("Reclaiming centre detail at a lower requested bitrate");ax.legend();ax.set_ylim(0,235);ax.grid(axis="y",alpha=.2);ax.set_axisbelow(True)
fig.savefig(p/"quality-budget.png",dpi=160)
