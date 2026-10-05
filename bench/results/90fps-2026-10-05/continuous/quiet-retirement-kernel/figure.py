#!/usr/bin/env python3
"""Plot one host functional run; no headset or latency inference."""
from pathlib import Path
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent
rows=[r for r in csv.DictReader((p/"outcomes.csv").open()) if r["mode"]=="normal"]
labels=["Both options enabled", "Recovery poll off", "Deadline off", "Non-ASTC control"]
fig,axs=plt.subplots(1,2,figsize=(10.4,4.5),layout="constrained")
for ax,key,title in zip(axs,["wait_ms","completed"],["First requested kernel-poll wait (ms)","Newer frame delivered to decoder stub"]):
 values=[int(r[key]) for r in rows];bars=ax.barh(labels,values,color=["#7452bd","#a5a5b5","#a5a5b5","#a5a5b5"])
 ax.invert_yaxis();ax.set_title(title,fontsize=11);ax.bar_label(bars,padding=3);ax.set_xlim(0,max(values)*1.25);ax.grid(axis="x",alpha=.2);ax.set_axisbelow(True)
fig.suptitle("Quiet retirement — actual caller and poll template, real localhost sockets",fontsize=12)
fig.supxlabel("One host functional run. Backdated fixture, stand-in clock/scene/decoder. No Pico latency result.",fontsize=8.5)
fig.savefig(p/"kernel-outcomes.png",dpi=170);fig.savefig(p/"kernel-outcomes.svg")
