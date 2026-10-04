from pathlib import Path
import json,statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p=Path(__file__).parent
rows=json.loads((p/"results.json").read_text())
labels=["Sync A", "Async B", "Sync C", "RGB + async D"]
fig,axes=plt.subplots(1,2,figsize=(12,4.6),layout="constrained")
for i,r in enumerate(rows):
 values=[v[3]/1000 for v in r["worker_stage_mean_us_windows"]]
 axes[0].scatter([i]*len(values),values,color="#8053d3",alpha=.45,s=22)
 axes[0].plot([i-.2,i+.2],[statistics.median(values)]*2,color="#17172b",linewidth=3)
 fps=r["render_iterations_per_second_windows"]
 axes[1].scatter([i]*len(fps),fps,color="#267a96",alpha=.6,s=22)
 axes[1].plot([i-.2,i+.2],[statistics.median(fps)]*2,color="#17172b",linewidth=3)
axes[0].set_ylabel("Host submission-to-handoff (ms)")
axes[0].set_title("Removed post-submit CPU wait")
axes[0].set_ylim(bottom=0)
axes[1].set_ylabel("Display-loop iterations / second")
axes[1].set_title("Stationary native-resolution wake captures")
axes[1].axhline(90,color="#68707a",linestyle="--",linewidth=1)
axes[1].set_ylim(84,92)
for a in axes:
 a.set_xticks(range(4),labels,rotation=12);a.grid(axis="y",alpha=.2)
fig.suptitle("Pico: shared-queue ASTC upload handoff\nDots are 180-frame worker means / 2-second render windows; lines are their medians")
fig.savefig(p/"upload-handoff.png",dpi=160)
fig.savefig(p/"upload-handoff.svg")
