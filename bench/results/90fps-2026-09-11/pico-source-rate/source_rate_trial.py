from pathlib import Path
import subprocess
r=Path(__file__).resolve().parent;live=r.parent/'motion-live';adb='/home/nerdrx/.local/bin/adb'
def restart(flag):subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned',flag],check=True)
try:
 for mode in ['60','off']:
  restart('--source60' if mode=='60' else '--source60-off')
  subprocess.run(['python3',str(live/'capture_live.py'),'pico-res100-source'+mode,'3','1000','20','0','3'],check=True)
finally:
 restart('--source60')
 subprocess.run([adb,'shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2','org.meumeu.wivrn.nx.warp'],check=True)
