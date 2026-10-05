#!/usr/bin/env python3
"""Plot deterministic caller fixture outputs; these are not wall-clock timings."""
from pathlib import Path
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent
rows=list(csv.DictReader((p/"caller.csv").open()))
labels=["Both options enabled", "Recovery poll off", "Deadline off", "Non-ASTC control"]
fig, ax=plt.subplots(1,2,figsize=(10,4.3),layout="constrained")
colors=["#7452bd", "#a5a5b5", "#a5a5b5", "#a5a5b5"]
for a,key,title in zip(ax,["first_wait_ms","decoder_stub_completed"],["First requested poll wait (virtual ms)","Newer frame delivered to decoder stub"]):
 values=[int(x[key]) for x in rows]
 bars=a.barh(labels,values,color=colors)
 a.invert_yaxis();a.set_title(title,fontsize=11);a.grid(axis="x",alpha=.2);a.set_axisbelow(True)
 a.bar_label(bars,padding=3);a.set_xlim(0,max(values)*1.25)
fig.suptitle("Quiet retirement scheduling — exact source caller projection",fontsize=13)
fig.supxlabel("Fake session/clock/scene/decoder. No network, headset or latency measurement.",fontsize=9)
fig.savefig(p/"caller-scheduling.png",dpi=170)
fig.savefig(p/"caller-scheduling.svg")
