from pathlib import Path
import subprocess,time,json
r=Path(__file__).resolve().parent;live=r.parent/'motion-live';adb='/home/nerdrx/.local/bin/adb'
def prop(k,v):subprocess.run([adb,'shell','setprop','debug.wivrn.nx.'+k,v],check=True)
try:
 prop('motion_trace','1')
 for n in ['0','1']:
  prop('motion_retain4',n)
  label='pico-retain-repeat'+n
  subprocess.run(['python3',str(live/'capture_live.py'),label,'3','1000','20','0','3'],check=True)
  data=subprocess.check_output(['python3',str(r/'analyze.py'),str(live/(label+'-client.log'))]);(r/(label+'.json')).write_bytes(data)
 prop('motion_trace','0')
 for n in ['0','1']:
  prop('motion_retain4',n);label='pico-retain-video'+n
  p=subprocess.Popen(['python3',str(live/'capture_live.py'),label,'3','1000','20','0','3'])
  time.sleep(8)
  remote='/sdcard/nx-retained-comparison.mp4'
  subprocess.run([adb,'shell','screenrecord','--time-limit','8','--size','1920x960',remote],check=True,timeout=20)
  subprocess.run([adb,'pull',remote,str(r/(label+'.mp4'))],check=True,stdout=subprocess.DEVNULL)
  subprocess.run([adb,'shell','rm',remote],check=True)
  p.wait(timeout=30)
finally:
 prop('motion_trace','0');prop('motion_retain4','1');prop('motion_past','1')
