from pathlib import Path
import subprocess,json
r=Path('/run/media/nerdrx/Lex/claude');out=r/'nx-scratch/no-group-cells';live=r/'nx-scratch/motion-live'
results=[]
for label,flag in [('no-groups-base-a','--no-groups-off'),('no-groups-candidate-a','--no-groups'),('no-groups-candidate-b','--no-groups'),('no-groups-base-b','--no-groups-off')]:
 pid=(live/'server-current.pid').read_text().strip()
 subprocess.run(['python3',str(live/'restart_server.py'),pid,'--force-owned',flag],check=True)
 subprocess.run(['python3',str(live/'capture_live.py'),label,'3','1000','60','0','5'],check=True)
 results.append(json.loads((live/(label+'-status.json')).read_text()));(out/'statuses.json').write_text(json.dumps(results,indent=2)+'\n')
