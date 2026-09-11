from pathlib import Path
import subprocess,shutil,os,json
r=Path('/run/media/nerdrx/Lex/claude');p=r/'nx-scratch/native-fit-live';live=r/'nx-scratch/motion-live';server=r/'nx-scratch/wivrn-atlas-live-current-build/server/wivrn-server';results=[]
try:
 for label,variant in [('fit-base-a','control'),('fit-skip-a','candidate'),('fit-skip-b','candidate'),('fit-base-b','control')]:
  tmp=server.with_name('wivrn-server.next');shutil.copy2(p/(variant+'-server'),tmp);os.replace(tmp,server)
  subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned','--large-centre','--no-groups','--r4-off'],check=True)
  subprocess.run(['python3',str(live/'capture_live.py'),label,'3','1000','60','0','3'],check=True)
  results.append(json.loads((live/(label+'-status.json')).read_text()));(p/'statuses.json').write_text(json.dumps(results,indent=2)+chr(10))
finally:
 tmp=server.with_name('wivrn-server.next');shutil.copy2(p/'candidate-server',tmp);os.replace(tmp,server)
