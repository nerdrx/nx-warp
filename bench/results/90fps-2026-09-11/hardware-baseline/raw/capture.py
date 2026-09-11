from pathlib import Path
import subprocess
r=Path('/run/media/nerdrx/Lex/claude');live=r/'nx-scratch/motion-live';base=r/'nx-scratch/live/xdg/wivrn/config.planar-gpu-graduated-copybaseline90.json'
def restart(config):subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned','--config',str(config)],check=True)
try:
 restart(r/'nx-scratch/hw-codec-probe/vaapi-hevc-2688-8bit.json')
 subprocess.run(['python3',str(live/'capture_live.py'),'hw-hevc8-capture','3','1000','25','76601','3'],check=True)
finally:
 restart(base)
 subprocess.run(['/home/nerdrx/.local/bin/adb','shell','setprop','debug.wivrn.nx.capture','0'],check=True)
 subprocess.run(['/home/nerdrx/.local/bin/adb','shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2','org.meumeu.wivrn.nx.warp'],check=True)
