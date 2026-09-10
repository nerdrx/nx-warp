from pathlib import Path
import subprocess,hashlib,re,json,statistics
p=Path(__file__).parent;a='/home/nerdrx/.local/bin/adb';b='/data/local/tmp/nx-sparse-layout/'
def call(args):return subprocess.run(args,capture_output=True,text=True,check=True)
call([a,'shell','am','force-stop','org.meumeu.wivrn.nx.warp']);call([a,'shell','mkdir','-p',b])
call([a,'push',str(p/'vkdec'),b+'vkdec']);call([a,'shell','chmod','755',b+'vkdec'])
for kind in ['static','motion']:call([a,'push',str(p.parent/'lite-bitcount'/f'{kind}.nxv'),b+kind+'.nxv'])
checks={};rows=[]
def command(kind,mode,out):
 env=['NXVC_VKD_PLANAR_FLAT=1','NXVC_VKD_EXACT_REUSE=0']
 if mode=='control':env+=['NXVC_VKD_PASSA_DYNAMIC_LAYOUT=1']
 return [a,'shell','env',*env,b+'vkdec','--in',b+kind+'.nxv',*out,'--format','ycbcr420','--compact-flat64','--unorm','0','--stats']
for kind in ['static','motion']:
 hashes=[]
 for mode in ['control','candidate']:
  r=call(command(kind,mode,['--out',b+'out.yuv']));(p/f'pixels-{kind}-{mode}.log').write_text(r.stdout+r.stderr)
  call([a,'pull',b+'out.yuv',str(p/'out.yuv')]);hashes.append(hashlib.sha256((p/'out.yuv').read_bytes()).hexdigest())
 assert hashes[0]==hashes[1];checks[kind]=hashes[0];print(kind,'pixel exact',flush=True)
for rep in range(4):
 for kind in ['static','motion']:
  for mode in (['control','candidate'] if rep%2==0 else ['candidate','control']):
   r=call(command(kind,mode,['--no-out']));(p/f'bench-{rep}-{kind}-{mode}.log').write_text(r.stdout+r.stderr)
   rr=re.findall(r'frame (\d+):.*?passA ([\d.]+).*?passW ([\d.]+).*?passB ([\d.]+).*?gpu ([\d.]+).*?total ([\d.]+)',r.stdout+r.stderr);rr=[list(map(float,x)) for x in rr if int(x[0])>=4];assert len(rr)==12
   row=dict(rep=rep,kind=kind,mode=mode,**{k:statistics.mean(x[i] for x in rr) for i,k in enumerate(['frame','passA','passW','passB','gpu','total']) if i});rows.append(row);print(row,flush=True)
(p/'results.json').write_text(json.dumps(dict(pixel_hashes=checks,runs=rows),indent=2)+'\n')
