#!/usr/bin/env python3
"""Plot standalone Pico CPU outcomes; no viewer latency inference."""
from pathlib import Path
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent
rows=[r for r in csv.DictReader((p/"outcomes.csv").open()) if r["trial"]=="initial"]
labels=["Both options enabled", "Recovery poll off", "Deadline off", "Non-ASTC control"]
fig,ax=plt.subplots(figsize=(8.8,3.8),layout="constrained")
values=[int(r["completed"]) for r in rows]
bars=ax.barh(labels,values,color=["#7452bd","#a5a5b5","#a5a5b5","#a5a5b5"])
ax.invert_yaxis();ax.set_xlim(0,1.35);ax.bar_label(bars,padding=3);ax.set_xticks([0,1]);ax.set_xlabel("Newer frame delivered to decoder stub (count)")
ax.set_title("Pico Android CPU / real localhost polling — functional outcomes",fontsize=12)
fig.supxlabel("Standalone fixture. Clock, scene, JNI and decoder substitutes. No viewer or latency measurement.",fontsize=8)
fig.savefig(p/"pico-outcomes.png",dpi=170);fig.savefig(p/"pico-outcomes.svg")
