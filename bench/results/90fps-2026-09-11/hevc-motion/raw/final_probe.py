from pathlib import Path
import subprocess
r=Path('/run/media/nerdrx/Lex/claude');live=r/'nx-scratch/motion-live';adb='/home/nerdrx/.local/bin/adb';base=r/'nx-scratch/live/xdg/wivrn/config.planar-gpu-graduated-copybaseline90.json'
def restart(cfg,flag):subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned','--config',str(cfg),flag],check=True)
subprocess.run([adb,'install','-r',str(r/'nx-scratch/wt-atlas-live-240/build/outputs/apk/release/wt-atlas-live-240-release.apk')],check=True)
try:
 restart(r/'nx-scratch/hw-codec-probe/vaapi-hevc-2688-10bit.json','--always-motion-field')
 subprocess.run([adb,'shell','setprop','debug.wivrn.nx.motion_mode','headset'],check=True)
 subprocess.run(['python3',str(live/'capture_live.py'),'hevc-motion-final','3','1000','30','0','3'],check=True)
 subprocess.run(['python3',str(live/'capture_live.py'),'hevc-motion-capture','3','1000','25','77701','3'],check=True)
finally:
 subprocess.run([adb,'shell','setprop','debug.wivrn.nx.motion_mode','default'],check=True)
 subprocess.run([adb,'shell','setprop','debug.wivrn.nx.capture','0'],check=True)
 restart(base,'--always-motion-field-off')
 subprocess.run([adb,'shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2','org.meumeu.wivrn.nx.warp'],check=True)
