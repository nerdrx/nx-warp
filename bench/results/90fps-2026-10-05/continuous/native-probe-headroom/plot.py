from pathlib import Path
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
root = Path(__file__).resolve().parent
rows = {m:list(csv.DictReader((root/f"{m}-late.csv").open())) for m in ("baseline","candidate")}
plt.rcParams.update({"font.size":11,"axes.spines.top":False,"axes.spines.right":False,"svg.fonttype":"none"})
fig, axes = plt.subplots(3,1,figsize=(12,10),gridspec_kw={"height_ratios":[1.1,1,1]})
colors={"baseline":"#bf5a39","candidate":"#6550a8"}
for m, data in rows.items():
    t=[int(r["desired_ns"])/1e9 for r in data]
    rate=[int(r["bitrate_bps"])/1e6 for r in data]
    serial=[int(r["serial_ns"])/1e6 for r in data]
    label="Original" if m=="baseline" else "Held candidate"
    axes[0].plot(t,rate,label=label,color=colors[m],lw=1.9)
    axes[1].plot(t,serial,color=colors[m],lw=1.9)
    axes[2].plot(t,rate,color=colors[m],lw=2.1)
for ax in axes[:2]:
    ax.axvspan(6,20,color="#e9c883",alpha=.2)
    ax.set_xlim(0,40);ax.grid(alpha=.17)
    ax.axvline(20,color="#607b81",ls=":",lw=1.3)
axes[0].set_ylabel("Requested media rate (Mbit/s)")
axes[0].set_ylim(0,1100)
axes[0].legend(loc="upper right")
axes[0].text(8,730,"Synthetic display-drop feedback",fontsize=10)
axes[0].text(21,110,"Capacity doubles at 20 s",fontsize=10)
axes[1].axhline(1000/90,color="#364d59",ls="--",lw=1.2,label="Nominal 90 Hz period (11.11 ms)")
axes[1].set_ylabel("Modeled serial eye service (ms)")
axes[1].set_ylim(0,24);axes[1].legend(loc="upper right")
axes[1].set_xlabel("Desired virtual frame start (s)")
axes[2].set_xlim(19.5,22);axes[2].set_ylim(250,900)
axes[2].axvline(20,color="#607b81",ls=":",lw=1.3)
axes[2].grid(alpha=.17)
axes[2].set_ylabel("Requested media rate (Mbit/s)")
axes[2].set_xlabel("Capacity-return detail: desired virtual frame start (s)")
axes[2].annotate("Existing slowdown rule triggers a transient cut",xy=(20.5,297.5),xytext=(20.6,450),fontsize=10,arrowprops={"arrowstyle":"->","color":"#6550a8"})
axes[2].text(19.65,870,"Original: 850 at 20.833 s    Candidate: 850 at 21.333 s",fontsize=10)
fig.suptitle("Native BBR probe guard: less overload, slower modeled recovery",fontsize=17,fontweight="bold",y=.98)
fig.text(.5,.94,"Actual controller and pacing helpers; synthetic link capacity and display-drop inputs",ha="center",fontsize=11)
fig.text(.07,.025,"Policy candidate held. Ideal serial service excludes real queues, drops, FEC, GPU work and feedback delay.\nThese curves do not measure headset FPS, latency, image quality or Wi-Fi capacity.",fontsize=10,color="#4b5563")
fig.subplots_adjust(left=.11,right=.96,bottom=.11,top=.9,hspace=.5)
fig.savefig(root/"probe-recovery.png",dpi=160)
fig.savefig(root/"probe-recovery.svg")
q=root/"probe-recovery.svg";q.write_text("\n".join(line.rstrip() for line in q.read_text().splitlines())+"\n")
