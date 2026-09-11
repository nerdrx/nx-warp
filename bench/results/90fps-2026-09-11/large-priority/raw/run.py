from pathlib import Path
import subprocess,json
root=Path('/run/media/nerdrx/Lex/claude');live=root/'nx-scratch/motion-live';out=root/'nx-scratch/large-priority'
adb='/home/nerdrx/.local/bin/adb'
states=[]
try:
 for label,priority in [('large-priority-on-a','1'),('large-priority-off-a','0'),('large-priority-off-b','0'),('large-priority-on-b','1')]:
  subprocess.run([adb,'shell','setprop','debug.wivrn.nx.decode_priority',priority],check=True)
  subprocess.run(['python3',str(live/'capture_live.py'),label,'3','1000','90','0','3'],check=True)
  state=json.loads((live/(label+'-status.json')).read_text());state['label']=label;state['priority']=priority;states.append(state)
  (out/'statuses.json').write_text(json.dumps(states,indent=2))
finally:
 subprocess.run([adb,'shell','setprop','debug.wivrn.nx.decode_priority','1'],check=True)
