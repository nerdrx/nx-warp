from pathlib import Path
import subprocess
r=Path('/run/media/nerdrx/Lex/claude');adb='/home/nerdrx/.local/bin/adb';control=r/'nx-scratch/dc-only/control.apk'
try:
 subprocess.run([adb,'install','-r',str(r/'nx-scratch/wt-atlas-live-240/build/outputs/apk/release/wt-atlas-live-240-release.apk')],check=True)
 subprocess.run(['python3',str(r/'nx-scratch/motion-live/capture_live.py'),'zero-candidate-a','3','1000','30','0','3'],check=True)
finally:
 subprocess.run([adb,'install','-r',str(control)],check=True)
subprocess.run(['python3',str(r/'nx-scratch/motion-live/capture_live.py'),'zero-base-b','3','1000','30','0','3'],check=True)
