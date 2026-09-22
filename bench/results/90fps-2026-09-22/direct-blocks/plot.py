"""Recreate readable charts from the recorded device output and payload metadata."""
from pathlib import Path
import json, re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
p=Path(__file__).resolve().parent
s=(p/"pico-smoothing.log").read_text()
rows=re.findall(r"([ABCD]) (baseline|smooth):.*?dispatch_ns_p50=(\d+) dispatch_ns_p95=(\d+)",s)
fig,axes=plt.subplots(1,2,figsize=(12,4.8))
ax=axes[0];x=np.arange(len(rows));med=[int(r[2])/1e6 for r in rows];tail=[int(r[3])/1e6 for r in rows]
ax.bar(x-.16,med,.32,color="#7700ff",label="Median")
ax.bar(x+.16,tail,.32,color="#be9aff",label="95th percentile")
ax.axhline(1000/90,color="#e65c42",ls="--",label="Entire 90 Hz budget")
ax.set_xticks(x,[r[0]+" "+r[1] for r in rows],rotation=15)
ax.set_ylabel("GPU dispatch time (ms)");ax.set_title("Pico / Adreno 650: manual smoothing is too costly")
ax.legend(fontsize=8)
ax=axes[1];m=json.loads((p/"metadata.json").read_text());a=json.loads((p/"astc-reference.json").read_text())
labels=["Custom, foveated","ASTC 12x12, full","Custom, full"]
values=[m["payload_bitrate_mbps"],a["stereo_payload_mbps_90hz_excluding_16byte_file_header"],m["unfoveated_payload_bitrate_mbps"]]
ax.barh(labels,values,color=["#7700ff","#9f6fff","#aaaaaa"])
ax.axvline(500,color="#e65c42",ls="--",label="500 Mbit/s target")
for i,v in enumerate(values):ax.text(v+20,i,f"{v:.0f}",va="center",fontsize=9)
ax.set_xlim(0,2250);ax.set_xlabel("Image payload (Mbit/s), before transport overhead");ax.set_title("2048² per eye, 90 fresh images/s");ax.legend(fontsize=8)
for ax in axes:ax.spines[["top","right"]].set_visible(False)
fig.suptitle("NX direct blocks — measured costs, not promised headset FPS",fontsize=14)
fig.tight_layout(rect=(0,.05,1,.95));fig.text(.5,.015,"Static resident fixture. Sampling timings exclude encoding, network, upload and presentation. ASTC uses a different bitrate.",ha="center",fontsize=8)
fig.savefig(p/"pico-costs.png",dpi=150)

# Four native-atlas repeats, not an ABBA comparison.
records=[]
for letter in "ABCD":
    line=(p/f"pico-atlas-{letter}.log").read_text()
    records.append({k:float(v)/1e6 for k,v in re.findall(r"(\w+_ns_p(?:50|95))=(\d+)",line)})
native=np.median([v["total_ns_p50"] for v in records]);native_tail=max(v["total_ns_p95"] for v in records)
plain=np.mean([int(v[2])/1e6 for v in rows if v[1]=="baseline"])
manual=np.mean([int(v[2])/1e6 for v in rows if v[1]=="smooth"])
fig,ax=plt.subplots(figsize=(9,4.8));vals=[plain,native,manual]
bars=ax.bar(["No smoothing\nkeep as baseline","Native filtering + atlas\nreject for now","Manual shader filtering\nreject"],vals,color=["#7700ff","#aaaaaa","#dddddd"])
for bar,v in zip(bars,vals):ax.text(bar.get_x()+bar.get_width()/2,v+.2,f"{v:.2f} ms",ha="center",fontweight="bold")
ax.axhline(1000/90,color="#e65c42",ls="--",label="Entire 90 Hz frame: 11.11 ms")
ax.set_ylim(0,18);ax.set_ylabel("Median GPU work per stereo image (ms)");ax.set_title("Pico decision: smoothing costs too much headroom")
ax.legend();ax.spines[["top","right"]].set_visible(False)
fig.text(.5,.035,"Native includes unpack + filtering; four repeat p95 values: 9.61–10.20 ms. No network or live presentation.",ha="center",fontsize=9)
fig.tight_layout(rect=(0,.075,1,1));fig.savefig(p/"smoothing-decision.png",dpi=150)
