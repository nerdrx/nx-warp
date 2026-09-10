from pathlib import Path
import subprocess,json
r=Path('/run/media/nerdrx/Lex/claude');out=r/'nx-scratch/r4-palette';live=r/'nx-scratch/motion-live';results=[]
for label,flag in [('r4-retry-base-a','--r4-off'),('r4-retry-candidate-a','--r4'),('r4-retry-candidate-b','--r4'),('r4-retry-base-b','--r4-off')]:
 pid=(live/'server-current.pid').read_text().strip()
 subprocess.run(['python3',str(live/'restart_server.py'),pid,'--force-owned','--no-groups',flag],check=True)
 subprocess.run(['python3',str(live/'capture_live.py'),label,'3','1000','60','0','5'],check=True)
 results.append(json.loads((live/(label+'-status.json')).read_text()));(out/'retry-statuses.json').write_text(json.dumps(results,indent=2)+'\n')
