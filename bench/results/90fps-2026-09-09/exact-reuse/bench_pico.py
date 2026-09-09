from pathlib import Path
import subprocess,re,json,statistics
p=Path(__file__).parent;a='/home/nerdrx/.local/bin/adb';base='/data/local/tmp/nx-exact/'
subprocess.run([a,'push',str(p.parent/'wide-ring/wide.nxv'),base+'local.nxv'],check=True,stdout=subprocess.DEVNULL)
results=[]
for repeat in range(4):
 for kind in ['static','local','motion']:
  for on in ([0,1] if repeat%2==0 else [1,0]):
   label=f'bench-{repeat}-{kind}-{on}'
   r=subprocess.run([a,'shell','env','NXVC_VKD_PLANAR_FLAT=1',f'NXVC_VKD_EXACT_REUSE={on}',base+'nxvc-vkdec','--in',base+kind+'.nxv','--no-out','--format','ycbcr420','--compact-centre','--unorm','0','--stats'],capture_output=True,text=True,check=True)
   (p/(label+'.log')).write_text(r.stdout+r.stderr)
   rows=re.findall(r'frame (\d+):.*?parse ([\d.]+).*?submit ([\d.]+).*?passA ([\d.]+).*?passW ([\d.]+).*?passB ([\d.]+).*?gpu ([\d.]+).*?total ([\d.]+)',r.stdout+r.stderr)
   data=[list(map(float,row)) for row in rows if int(row[0])>=4];assert len(data)>=12
   means={k:statistics.mean(x[i] for x in data) for i,k in enumerate(['frame','parse','submit','passA','passW','passB','gpu','total']) if i}
   results.append(dict(repeat=repeat,kind=kind,reuse=on,samples=len(data),**means));print(results[-1],flush=True)
(p/'benchmark.json').write_text(json.dumps(results,indent=2)+'\n')
