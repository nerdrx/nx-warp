from pathlib import Path
import subprocess,hashlib,json,re
p=Path(__file__).parent;a='/home/nerdrx/.local/bin/adb';base='/data/local/tmp/nx-exact/'
def run(args,log):
 with (p/log).open('w') as f:subprocess.run(args,stdout=f,stderr=subprocess.STDOUT,check=True)
subprocess.run([a,'shell','chmod','755',base+'nxvc-vkdec'],check=True)
checks={}
for kind in ['static','motion','qp']:
 hashes=[]
 for on in [0,1]:
  label=f'{kind}-{on}'
  run([a,'shell','env','NXVC_VKD_PLANAR_FLAT=1',f'NXVC_VKD_EXACT_REUSE={on}','NXVC_VKD_EXACT_REUSE_TRACE=1',base+'nxvc-vkdec','--in',base+kind+'.nxv','--out',base+'out.yuv','--format','ycbcr420','--compact-centre','--unorm','0','--stats'],label+'.log')
  run([a,'pull',base+'out.yuv',str(p/(label+'.yuv'))],label+'-pull.log')
  hashes.append(hashlib.sha256((p/(label+'.yuv')).read_bytes()).hexdigest())
 assert hashes[0]==hashes[1],(kind,hashes)
 rows=re.findall(r'exact-reuse: frame (\d+) reused (\d+) eligible (\d+)',(p/(kind+'-1.log')).read_text());assert len(rows)==16,(kind,len(rows))
 assert int(rows[0][1])==0
 checks[kind]={'sha256':hashes[0],'all_frames_exact':True,'trace':[list(map(int,r)) for r in rows]}
 if kind=='static':assert all(int(r[1])==int(r[2])>0 for r in rows[1:])
(p/'checks.json').write_text(json.dumps(checks,indent=2)+'\n');print({k:{x:y for x,y in v.items() if x!='trace'} for k,v in checks.items()})
