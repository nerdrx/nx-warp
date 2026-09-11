from pathlib import Path
import subprocess
r=Path('/run/media/nerdrx/Lex/claude');live=r/'nx-scratch/motion-live';base=r/'nx-scratch/live/xdg/wivrn/config.planar-gpu-graduated-copybaseline90.json'
def restart(config):subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned','--config',str(config)],check=True)
def trial(label):subprocess.run(['python3',str(live/'capture_live.py'),label,'3','1000','30','0','3'],check=True)
trial('hw-nx-base-a')
try:
 restart(r/'nx-scratch/hw-codec-probe/vaapi-hevc-2688.json');trial('hw-hevc-a')
finally:restart(base)
trial('hw-nx-base-b')
