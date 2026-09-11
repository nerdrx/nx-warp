from pathlib import Path
import subprocess,time,os
r=Path(__file__).resolve().parent;live=r.parent/'motion-live';adb='/home/nerdrx/.local/bin/adb'
def restart(flag):subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned',flag],check=True)
os.environ['NX_REQUIRE_FOCUSED']='1'
try:
 for mode in ['60','off']:
  restart('--source60' if mode=='60' else '--source60-off');label='pico-source-video'+mode
  p=subprocess.Popen(['python3',str(live/'capture_live.py'),label,'3','1000','20','0','3'])
  time.sleep(8);remote='/sdcard/nx-source-rate-video.mp4'
  subprocess.run([adb,'shell','screenrecord','--time-limit','8','--size','1920x960',remote],check=True,timeout=20)
  subprocess.run([adb,'pull',remote,str(r/(label+'.mp4'))],check=True,stdout=subprocess.DEVNULL)
  subprocess.run([adb,'shell','rm',remote],check=True);p.wait(timeout=30)
  if p.returncode: raise RuntimeError('Visual trial failed; inspect focus status and capture')
finally:
 restart('--source60');subprocess.run([adb,'shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2','org.meumeu.wivrn.nx.warp'],check=True)
