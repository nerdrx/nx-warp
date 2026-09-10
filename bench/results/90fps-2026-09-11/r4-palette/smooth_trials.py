from pathlib import Path
import subprocess,json
r=Path('/run/media/nerdrx/Lex/claude');out=r/'nx-scratch/r4-palette';live=r/'nx-scratch/motion-live';results=[]
for label,smooth in [('r4-smooth5-a','5'),('r4-smooth3-a','3'),('r4-smooth3-b','3'),('r4-smooth5-b','5')]:
 pid=(live/'server-current.pid').read_text().strip()
 subprocess.run(['python3',str(live/'restart_server.py'),pid,'--force-owned','--no-groups','--r4'],check=True)
 subprocess.run(['python3',str(live/'capture_live.py'),label,'3','1000','60','0',smooth],check=True)
 results.append(json.loads((live/(label+'-status.json')).read_text()));(out/'smooth-statuses.json').write_text(json.dumps(results,indent=2)+'\n')
