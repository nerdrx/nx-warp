from pathlib import Path
import subprocess
r=Path('/run/media/nerdrx/Lex/claude');live=r/'nx-scratch/motion-live';adb='/home/nerdrx/.local/bin/adb'
base=r/'nx-scratch/live/xdg/wivrn/config.planar-gpu-graduated-copybaseline90.json';hevc=r/'nx-scratch/hw-codec-probe/vaapi-hevc-2688-10bit.json'
def restart(cfg,*flags):
 subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned','--config',str(cfg),*flags],check=True)
try:
 subprocess.run([adb,'shell','setprop','debug.wivrn.nx.motion_mode','headset'],check=True)
 restart(hevc,'--always-motion-field','--source60')
 subprocess.run(['python3',str(live/'capture_live.py'),'hevc-rescue-control','3','1000','30','0','3'],check=True)
 with (r/'nx-scratch/motion-gpu-truth/server-build.log').open('w') as f:subprocess.run(['cmake','--build',str(r/'nx-scratch/wivrn-atlas-live-current-build'),'--target','wivrn-server','-j6'],stdout=f,stderr=subprocess.STDOUT,check=True)
 restart(hevc,'--always-motion-field','--source60')
 subprocess.run(['python3',str(live/'capture_live.py'),'hevc-rescue-on','3','1000','30','0','3'],check=True)
finally:
 subprocess.run([adb,'shell','setprop','debug.wivrn.nx.motion_mode','default'],check=True)
 restart(base,'--always-motion-field-off','--source60-off','--motion-clock-off')
 subprocess.run([adb,'shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2','org.meumeu.wivrn.nx.warp'],check=True)
