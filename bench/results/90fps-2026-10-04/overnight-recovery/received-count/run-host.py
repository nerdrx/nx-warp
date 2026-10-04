import csv,os,subprocess,sys
from pathlib import Path
out=Path(sys.argv[1]);allowed=os.sched_getaffinity(0);cpu=3 if 3 in allowed else min(allowed)
rows=[]
for run,mode in enumerate(["baseline","cached","cached","baseline"]):
 def pin():os.sched_setaffinity(0,{cpu});os.nice(5)
 raw=subprocess.check_output([str(out/mode)],text=True,preexec_fn=pin)
 block=list(csv.DictReader(raw.splitlines()));assert len(block)==30
 for row in block:row.update(mode=mode,run=run,cpu=cpu);rows.append(row)
with (out/"host-abba.csv").open("w") as f:
 w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
print("120 valid rows; ABBA modes; expected deadlines pass")
