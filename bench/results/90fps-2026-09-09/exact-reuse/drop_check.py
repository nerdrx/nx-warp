from pathlib import Path
import subprocess,json,hashlib
p=Path(__file__).parent;a='/home/nerdrx/.local/bin/adb';b='/data/local/tmp/nx-exact/'
checks={}
for kind in ['static','motion','qp']:
 for on in [0,1]:
  r=subprocess.run([a,'shell','env','NXVC_VKD_PLANAR_FLAT=1',f'NXVC_VKD_EXACT_REUSE={on}','NXVC_VKD_EXACT_REUSE_TRACE=1',b+'nxvc-vkdec','--in',b+kind+'.nxv','--out',b+'drop.yuv','--decode-every','2','--format','ycbcr420','--compact-centre','--unorm','0'],capture_output=True,text=True,check=True)
  (p/f'drop-{kind}-{on}.log').write_text(r.stdout+r.stderr)
  subprocess.run([a,'pull',b+'drop.yuv',str(p/'drop.yuv')],check=True,stdout=subprocess.DEVNULL)
  full=(p/f'{kind}-0.yuv').read_bytes();n=1856*928*3//2
  expected=b''.join(full[i*n:(i+1)*n] for i in range(0,16,2));actual=(p/'drop.yuv').read_bytes();assert actual==expected,(kind,on)
 checks[kind]={'every_second_frame_exact':True,'frames':8}
(p/'drop-checks.json').write_text(json.dumps(checks,indent=2)+'\n');print(checks)
