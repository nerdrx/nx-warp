import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parent
fig,ax=plt.subplots(figsize=(8,4.5))
for mode,label,color in [("baseline","1 MiB ring","#788296"),("candidate","2 MiB ring","#5623a6")]:
 rows=[r for r in csv.DictReader((root/(mode+".csv")).open())if r["fec"]=="1"and r["age_frames"]=="2"]
 x=[int(r["aggregate_payload_mbps"])for r in rows];y=[100*int(r["retained_payload_bytes"])/int(r["frame_payload_bytes"])for r in rows]
 ax.plot(x,y,"o-",label=label,color=color,linewidth=2)
 for a,b in zip(x,y):ax.annotate(f"{b:.1f}%",(a,b),xytext=(0,-17 if mode=="candidate" else 8),textcoords="offset points",ha="center",fontsize=9)
ax.set_xticks([250,500,700,1000]);ax.set_ylim(-5,110);ax.set_xlabel("Aggregate synthetic stereo payload budget (Mbit/s)");ax.set_ylabel("Two-frame-old payload still held (%)");ax.spines[["top","right"]].set_visible(False);ax.legend(loc="lower left")
fig.suptitle("Keep repair bytes available at higher payload rates")
fig.text(.5,.025,"Actual history + FEC serialization; equal eyes, constant frames, 90 source FPS. No network or headset timing.",ha="center",fontsize=8)
fig.tight_layout(rect=(0,.07,1,.93));fig.savefig(root/"retention.png",dpi=160)
