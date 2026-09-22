import os,subprocess,time,sys
from pathlib import Path
p=Path('/run/media/nerdrx/Lex/claude/nx-scratch/live/direct-20260922')
adb=['/home/nerdrx/.local/bin/adb']
tag,loss=sys.argv[1:3]
subprocess.run(adb+['shell','setprop','debug.wivrn.nx.safety_loss_test',loss],check=True)
try:
 subprocess.run(['python3','/tmp/nx-live-probe.py',tag,'160'],env=dict(os.environ,NX_PROBE_CONFIG=str(p/'config-safety-test.json')),check=True)
 time.sleep(8)
 with (p/(tag+'.png')).open('wb') as f: subprocess.run(adb+['exec-out','screencap','-p'],stdout=f,check=True)
 time.sleep(6)
finally:
 subprocess.run(['python3','/tmp/nx-live-probe.py','stop'])
 subprocess.run(adb+['shell','setprop','debug.wivrn.nx.safety_loss_test','0'])
