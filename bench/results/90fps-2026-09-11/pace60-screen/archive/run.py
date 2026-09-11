from pathlib import Path
import subprocess,time
r=Path('/run/media/nerdrx/Lex/claude');live=r/'nx-scratch/motion-live'
while not (live/'zero-base-b-status.json').exists():time.sleep(1)
base=r/'nx-scratch/live/xdg/wivrn/config.planar-gpu-graduated-copybaseline90.json'
def restart(config):
 subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned','--config',str(config)],check=True)
try:
 restart(r/'nx-scratch/pace-probe/pace60.json')
 subprocess.run(['python3',str(live/'capture_live.py'),'pace60-a','3','1000','30','0','3'],check=True)
finally:restart(base)
subprocess.run(['python3',str(live/'capture_live.py'),'pace-auto-b','3','1000','30','0','3'],check=True)
