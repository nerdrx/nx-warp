from pathlib import Path
import subprocess
r=Path('/run/media/nerdrx/Lex/claude');live=r/'nx-scratch/motion-live'
def restart(flag):subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned',flag],check=True)
try:
 restart('--native-tskip')
 subprocess.run(['python3',str(live/'capture_live.py'),'tskip-on-a','3','1000','30','0','3'],check=True)
finally:restart('--native-tskip-off')
subprocess.run(['python3',str(live/'capture_live.py'),'tskip-off-b','3','1000','30','0','3'],check=True)
