from pathlib import Path
import subprocess,json
r=Path('/run/media/nerdrx/Lex/claude');out=r/'nx-scratch/r4-palette';live=r/'nx-scratch/motion-live';adb='/home/nerdrx/.local/bin/adb';results=[]
for label,size,large in [('speed-large2688','2688',True),('speed-small2688','2688',False),('speed-small2176','2176',False)]:
 for key,value in [('test_eye_size',size),('compact_large_centre','1' if large else '0'),('transport_sha2','0'),('compact_flat64','1')]:subprocess.run([adb,'shell','setprop','debug.wivrn.nx.'+key,value],check=True)
 subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned','--no-groups','--r4-off','--large-centre' if large else '--large-centre-off'],check=True)
 subprocess.run(['python3',str(live/'capture_live.py'),label,'3','1000','90','0','3'],check=True)
 results.append(json.loads((live/(label+'-status.json')).read_text()));(out/'speed-statuses.json').write_text(json.dumps(results,indent=2)+'\n')
