from pathlib import Path
import subprocess,json,time
r=Path('/run/media/nerdrx/Lex/claude');out=r/'nx-scratch/plane-split';live=r/'nx-scratch/motion-live';adb='/home/nerdrx/.local/bin/adb'
while True:
 log=(out/'lds-apk-build.log').read_text(errors='replace')
 if 'BUILD FAILED' in log:raise SystemExit('Build failed')
 if 'BUILD SUCCESSFUL' in log:break
 time.sleep(3)
results=json.loads((out/'results.json').read_text());assert results['r2_equal'] and results['r4_equal']
subprocess.run([adb,'install','-r',str(r/'nx-scratch/wt-atlas-live-240/build/outputs/apk/release/wt-atlas-live-240-release.apk')],check=True)
results=[]
try:
 for label,split in [('lds-base-a','0'),('lds-split-a','1'),('lds-split-b','1'),('lds-base-b','0')]:
  subprocess.run([adb,'shell','setprop','debug.wivrn.nx.native_plane_split',split],check=True)
  subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned','--large-centre','--no-groups','--r4-off'],check=True)
  subprocess.run(['python3',str(live/'capture_live.py'),label,'3','1000','60','0','3'],check=True)
  results.append(json.loads((live/(label+'-status.json')).read_text()));(out/'lds-statuses.json').write_text(json.dumps(results,indent=2)+'\n')
finally:
 subprocess.run([adb,'shell','setprop','debug.wivrn.nx.native_plane_split','0'],check=True)
