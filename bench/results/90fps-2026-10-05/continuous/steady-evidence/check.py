import csv
from pathlib import Path
from collections import defaultdict
root=Path(__file__).resolve().parent
old=root.parent/"noisy-feedback/raw/D/rows.csv"
groups={}
summary=[]
for mode,p in [("D",old),("F",root/"raw/F/rows.csv")]:
    g=defaultdict(list)
    with p.open() as f:
        for r in csv.DictReader(f):g[r["scenario"],int(r["phase"])].append(r)
    assert len(g)==34
    groups[mode]=g
    for (case,phase),rows in g.items():
        assert len(rows)==1800 and [int(r["frame"]) for r in rows]==list(range(1800))
        v=[int(r["bitrate_bps"]) for r in rows]
        assert min(v)>=10_000_000 and max(v)<=50_000_000
        summary.append([mode,case,phase,min(v),max(v),v[-1],sum(x>24_000_000 for x in v),sum(x<20_400_000 for x in v)])
D=root.parent/"material-recovery-bound/raw/capacity"
results=[]
for step in [3,5,10,20]:
    times={}
    for mode,folder in [("D",D),("F",root/"gates-F/capacity")]:
        with (folder/f"rise{step}-clean.csv").open() as f:rows=list(csv.DictReader(f))
        assert len(rows)==3600
        times[mode]=next(int(x["start_ns"])/1e9 for x in rows if int(x["start_ns"])>=step*1e9 and int(x["bitrate_bps"])>=840_000_000)
    if step!=20:assert times["D"]==times["F"]
    else:assert times["F"]-times["D"]>3.0
    results.append([step,times["D"],times["F"],times["F"]-times["D"]])
for phase in [0,360]:
    for case in ["moderate-150","repeated-150"]:
        v=[int(x["bitrate_bps"]) for x in groups["F"][case,phase]]
        assert max(v)<=26_400_002 and min(v)>=20_400_000
for name,header,rows in [("summary.csv",["mode","scenario","phase","min_bps","max_bps","final_bps","frames_above_24Mbps","frames_below_20_4Mbps"],summary),("recovery.csv",["rise_at_virtual_s","D_first_840Mbps_at_s","F_first_840Mbps_at_s","delay_virtual_s"],results)]:
    with (root/name).open("w",newline="") as f:
        w=csv.writer(f,lineterminator="\n");w.writerow(header);w.writerows(rows)
print("Reproduced comparisons: F limits selected 1.5x bursts, but 20s clean recovery delays by 3.067 virtual seconds. F held.")
