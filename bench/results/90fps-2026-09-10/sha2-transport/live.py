from pathlib import Path
import subprocess,sys
p=Path(__file__).parent;root=p.parent.parent;a='/home/nerdrx/.local/bin/adb';live=root/'nx-scratch/motion-live'
def run(args):subprocess.run(args,check=True)
try:
 run([a,'install','-r',str(p/'candidate.apk')])
 for label in ['sha2-transport-candidate-a','sha2-transport-candidate-b']:
  run([sys.executable,str(live/'capture_live.py'),label,'3','1000','60','0','3'])
 run([a,'install','-r',str(p/'control.apk')])
 run([sys.executable,str(live/'capture_live.py'),'sha2-transport-base-b','3','1000','60','0','3'])
finally:
 run([a,'install','-r',str(p/'control.apk')])
 run([a,'shell','setprop','pvr.factorytest.never.sleep','0'])
 run([a,'shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2','org.meumeu.wivrn.nx.warp'])
