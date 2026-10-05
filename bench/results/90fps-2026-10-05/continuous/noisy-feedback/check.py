import csv
from pathlib import Path
from collections import defaultdict
root=Path(__file__).resolve().parent
modes=[x for x in ["baseline","D","E"] if (root/"raw"/x/"rows.csv").exists()]
summary=[]
for mode in modes:
    groups=defaultdict(list)
    for r in csv.DictReader((root/"raw"/mode/"rows.csv").open()):groups[r["scenario"],int(r["phase"])].append(r)
    assert len(groups)==34
    for (case,phase),rows in groups.items():
        assert len(rows)==1800 and [int(r["frame"]) for r in rows]==list(range(1800))
        elapsed=[int(r["elapsed_ns"]) for r in rows]
        assert all(b>a for a,b in zip(elapsed,elapsed[1:]))
        if case.startswith("gap-"):
            gap=12 if case.startswith("gap-12") else 5 if case.startswith("gap-5") else 1
            assert elapsed[45]-elapsed[44]==gap*1_000_000_000+11_111_111
        rates=[int(r["bitrate_bps"]) for r in rows]
        assert all(10_000_000<=v<=50_000_000 for v in rates)
        if case.startswith("records-then-collapse"):
            assert abs(rates[-1]-10_200_000)<=2
        if case=="loss-after-records":assert min(rates[360:540])<rates[359]
        summary.append([mode,case,phase,min(rates),max(rates),rates[-1],sum(v>24_000_000 for v in rates),sum(v<20_400_000 for v in rates)])
with (root/"summary.csv").open("w",newline="") as f:
    w=csv.writer(f,lineterminator="\n")
    w.writerow(["mode","scenario","phase","min_bps","max_bps","final_bps","frames_target_above_24Mbps","frames_target_below_20_4Mbps"])
    w.writerows(summary)
print(f"Noisy replay structure and safety controls pass: {len(modes)} modes, 34 cases/mode, 1800 rows/case.")

# Candidate E keeps all eight clean/late rise traces, including first >=840 Mbps timing.
D=root.parent/"material-recovery-bound/raw"
for case in [f"rise{step}-{kind}.csv" for step in [3,5,10,20] for kind in ["clean","late"]]+["fall250-clean.csv"]:
    assert (root/"gates-E/capacity"/case).read_bytes()==(D/"capacity"/case).read_bytes()
def read(p):
    with p.open() as f:return list(csv.DictReader(f))
a=read(D/"capacity/fall300-clean.csv");b=read(root/"gates-E/capacity/fall300-clean.csv")
assert len(a)==len(b)==3600
for x,y in zip(a,b):
    assert all(x[k]==y[k] for k in x if k!="bitrate_bps")
    assert abs(int(x["bitrate_bps"])-int(y["bitrate_bps"]))<=47
old=read(D/"probe-phase/summary.csv");new=read(root/"gates-E/probe-phase/summary.csv")
assert len(old)==len(new)==48
old_control=next(r for r in old if r["burst_offset"]=="-1")
new_control=next(r for r in new if r["burst_offset"]=="-1")
assert all(old_control[k]==new_control[k] for k in old_control if k!="phase_max_bps")
assert int(old_control["phase_max_bps"])-int(new_control["phase_max_bps"])==2
assert max(int(r["phase_max_bps"]) for r in new)<=26_400_002
assert max(int(r["bitrate_bps"]) for r in read(root/"gates-E/phase/rows.csv"))<=24_000_000
lookup={(x[0],x[1],x[2]):x for x in summary}
assert lookup["E","moderate-125",360][4]<lookup["D","moderate-125",360][4]
assert lookup["E","moderate-150",360][4]<lookup["D","moderate-150",360][4]
assert lookup["E","moderate-150",0][4]>24_000_000 # remaining steady-state growth limitation
print("Analysis checks pass. Strict byte-exact E acceptance fails: <=47 bps collapse rounding and 2 bps no-burst peak. Steady growth remains unresolved; E is held.")
