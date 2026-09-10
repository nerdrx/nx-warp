from pathlib import Path
import json,re,subprocess,sys
p=Path(__file__).parent
log=Path(sys.argv[1])
text=log.read_text()
rows=[]
for line in text.splitlines():
 if 'PxrMetric:' not in line:continue
 m=re.search(r'GPUTemp=([0-9.]+)C',line)
 if m:rows.append(float(m[1]))
r={'vendor_gpu_temperature_C':{'n':len(rows),'min':min(rows) if rows else None,'max':max(rows) if rows else None,'first':rows[0] if rows else None,'last':rows[-1] if rows else None},'telemetry':json.loads(subprocess.check_output([sys.executable,str(p/'analyze_warm.py'),str(log)],text=True)),'coverage':json.loads(subprocess.check_output([sys.executable,str(p/'analyze_stability.py'),str(log)],text=True))}
(p/'soak-summary.json').write_text(json.dumps(r,indent=2)+'\n')
print(json.dumps(r))
