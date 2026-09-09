from pathlib import Path
import subprocess,json,sys
root=Path('/run/media/nerdrx/Lex/claude');live=root/'nx-scratch/motion-live';out=Path(__file__).parent
adb='/home/nerdrx/.local/bin/adb';pkg='org.meumeu.wivrn.nx.warp';statuses=[]
try:
 for label,mode in [('remap-math-3a','3'),('remap-math-4a','4'),('remap-math-4b','4'),('remap-math-3b','3')]:
  subprocess.run([sys.executable,str(live/'capture_live.py'),label,'1','4000','60','0',mode],cwd=root,check=True)
  statuses.append(json.loads((live/f'{label}-status.json').read_text()));(out/'statuses.json').write_text(json.dumps(statuses,indent=2)+'\n')
finally:
 subprocess.run([adb,'shell','am','force-stop',pkg],check=True)
 profile=json.loads((live/'ACTIVE_USER_PROFILE.json').read_text())
 for k,v in profile['properties'].items():subprocess.run([adb,'shell','setprop','debug.wivrn.nx.'+k,v],check=True)
 subprocess.run([adb,'shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2',pkg],check=True)
