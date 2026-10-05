from pathlib import Path
import csv,json,statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent
current=json.loads((p/"checked-summary.json").read_text())["metrics"]
old=list(csv.DictReader((p.parent/"stereo-gpu/build/results/stereo.csv").open()))
old_wait=statistics.median(float(r["eye0_wait_ms"]) for r in old if r.get("mode")=="serial")
plt.rcParams.update({"font.size":11,"figure.facecolor":"#f8f7fc","axes.facecolor":"#f8f7fc"})
fig,axs=plt.subplots(1,2,figsize=(10.5,4.8))
vals=[old_wait,current["eye0_wait_ms"]["p50_lower_rank"]]
axs[0].bar(["Earlier run","Current recheck"],vals,color=["#9295a2","#7e57c2"]);axs[0].set_ylabel("First-eye fence call p50 (ms)");axs[0].set_ylim(0,11)
for i,b in enumerate(vals):axs[0].text(i,b+.15,f"{b:.3f}",ha="center")
labels=["Complete call","Eye0 packet","Eye1 packet","Eye0 wait"]
vals=[current[m]["p50_lower_rank"] for m in ["wall_ms","eye0_pack_ms","eye1_pack_ms","eye0_wait_ms"]]
axs[1].barh(labels,vals,color=["#273955","#51a6a6","#51a6a6","#7e57c2"]);axs[1].invert_yaxis();axs[1].set_xlabel("Current p50 wall intervals (ms)");axs[1].set_xlim(0,6.5)
for i,b in enumerate(vals):axs[1].text(b+.07,i,f"{b:.3f}",va="center")
for ax in axs:ax.spines[["top","right"]].set_visible(False)
fig.suptitle("The earlier ~9 ms fence wait did not reproduce",fontweight="bold")
fig.text(.5,.035,"Different load observations, not a code-change speedup • native2176²/slot • same archived ASTC payloads\nCurrent20 offscreen calls; no live compositor, network, Pico or photon measurement.",ha="center",fontsize=9)
fig.tight_layout(rect=(0,.12,1,.92));fig.savefig(p/"fence-recheck.png",dpi=170)
