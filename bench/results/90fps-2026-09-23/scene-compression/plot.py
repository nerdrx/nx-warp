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

import csv
dec=list(csv.reader((p/"pico-production-decode.csv").open()))
fig,ax=plt.subplots(figsize=(8,4),layout="constrained")
for i,(method,label,color) in enumerate([("lz4","LZ4","#888888"),("zstd3","Zstd level 3","#7700ff")]):
 r=[v for v in dec if v[1]==method]; med=np.array([float(v[4]) for v in r]); hi=np.array([float(v[5]) for v in r])
 ax.bar(x+(i-.5)*.32,med,.32,label=label,color=color,yerr=np.array([np.zeros(3),hi-med]),capsize=4)
ax.set_xticks(x,[r["scene"].capitalize() for r in rows]);ax.set_ylim(0,.5);ax.set_ylabel("CPU decode milliseconds (median; whisker = p95)");ax.set_title("Pico: production lossless decode helpers, isolated");ax.legend();ax.grid(axis="y",alpha=.2);ax.set_axisbelow(True)
fig.savefig(p/"pico-decode.png",dpi=160)
