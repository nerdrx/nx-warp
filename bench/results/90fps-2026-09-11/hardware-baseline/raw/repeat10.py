from pathlib import Path
import subprocess,time
r=Path('/run/media/nerdrx/Lex/claude');live=r/'nx-scratch/motion-live';base=r/'nx-scratch/live/xdg/wivrn/config.planar-gpu-graduated-copybaseline90.json'
while not (live/'hw-nx-base-b-status.json').exists():time.sleep(1)
def restart(config):subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned','--config',str(config)],check=True)
try:
 restart(r/'nx-scratch/hw-codec-probe/vaapi-hevc-2688-10bit.json')
 for label in ['hw-hevc10-a','hw-hevc10-b']:
  subprocess.run(['python3',str(live/'capture_live.py'),label,'3','1000','30','0','3'],check=True)
finally:
 restart(base)
 subprocess.run(['/home/nerdrx/.local/bin/adb','shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2','org.meumeu.wivrn.nx.warp'],check=True)
