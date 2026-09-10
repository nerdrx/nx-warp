from pathlib import Path
import subprocess,sys,json,hashlib
p=Path(__file__).parent;root=p.parent.parent;live=root/'nx-scratch/motion-live';a='/home/nerdrx/.local/bin/adb';pkg='org.meumeu.wivrn.nx.warp'
apks={'base':p.parent/'sparse-layout/control.apk','candidate':p/'candidate.apk'}
def run(args):subprocess.run(args,check=True)
(p/'apk-sha256.json').write_text(json.dumps({k:hashlib.sha256(v.read_bytes()).hexdigest() for k,v in apks.items()},indent=2))
try:
 previous=None
 for suffix,mode in [('base-a','base'),('candidate-a','candidate'),('candidate-b','candidate'),('base-b','base')]:
  if mode!=previous:run([a,'install','-r',str(apks[mode])]);previous=mode
  label='sparse-layout-long-'+suffix
  print('START',label,flush=True)
  run([sys.executable,str(live/'capture_live.py'),label,'3','1000','120','0','3'])
  print('DONE',label,flush=True)
finally:
 run([a,'install','-r',str(apks['candidate'])])
 run([a,'shell','setprop','pvr.factorytest.never.sleep','0'])
 run([a,'shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2',pkg])
