from pathlib import Path
import json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p=Path(__file__).parent
rows=json.loads((p/"summary.json").read_text());x=np.arange(len(rows));fig,ax=plt.subplots(figsize=(10,4.5),layout="constrained")
for i,(key,label,color) in enumerate([("rgb888_lz4","Reference: RGB888 + LZ4","#888888"),("rgb888_zstd3","Exact same pixels + Zstd3","#7700ff"),("rgb565_zstd3","Small colour tradeoff + Zstd3","#00a5b5")]):
 ax.bar(x+(i-1)*.25,[r[key]/1000 for r in rows],.25,label=label,color=color)
ax.set_xticks(x,[r["scene"].capitalize() for r in rows]);ax.set_ylabel("Detail envelope (kB per duplicated-eye frame)");ax.set_title("Fixed previous 500 Mbit/s allocation — static scene fixtures");ax.legend();ax.grid(axis="y",alpha=.2);ax.set_axisbelow(True)
fig.savefig(p/"comparison.png",dpi=160)
