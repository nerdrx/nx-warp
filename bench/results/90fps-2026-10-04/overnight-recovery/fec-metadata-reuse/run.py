import csv,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
os.sched_setaffinity(0,{3})
assert os.sched_getaffinity(0)=={3}
os.nice(5)
for probe,columns in [("recovery",["k","stride","missing_position","max_blob_bytes","mean_ns","allocations","allocated_bytes"]),("encode",["payload_bytes","metadata_mask","blob_bytes","mean_ns","allocations","allocated_bytes"])]:
 with (root/(probe+".csv")).open("w",newline="") as f:
  w=csv.writer(f,lineterminator="\n");w.writerow(["block","position","treatment","seed"]+columns)
  for block in range(5):
   for pos,treatment in enumerate(["baseline","candidate","candidate","baseline"]):
    result=subprocess.run([str(root/(probe+"-"+treatment)),str(23000+block)],check=True,capture_output=True,text=True)
    for line in result.stdout.splitlines():w.writerow([block,pos,treatment,23000+block]+line.split(","))
 print(probe,"done")
