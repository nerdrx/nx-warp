import subprocess,json,hashlib
from pathlib import Path
p=Path(__file__).parent;a='/home/nerdrx/.local/bin/adb';base='/data/local/tmp/nx-sha-'
def run(args):return subprocess.run(args,check=True,capture_output=True)
run([a,'shell','am','force-stop','org.meumeu.wivrn.nx.warp'])
for mode in ['control','candidate']:run([a,'push',str(p/mode),base+mode]);run([a,'shell','chmod','755',base+mode])
checks={}
for mode,env in [('control',[]),('candidate',['NXT_SHA256_ACCELERATE=1']),('portable',['NXT_SHA256_ACCELERATE=1','NXT_SHA256_PORTABLE=1'])]:
 exe='candidate' if mode=='portable' else mode
 run([a,'shell','env',*env,base+exe,'>',base+'output.bin','2>',base+'check.log']);run([a,'pull',base+'output.bin',str(p/'output.bin')]);run([a,'pull',base+'check.log',str(p/f'{mode}-checks.log')]);data=(p/'output.bin').read_bytes();checks[mode]=dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
assert len({x['sha256'] for x in checks.values()})==1
rows=[]
for rep in range(4):
 for mode in (['control','candidate'] if rep%2==0 else ['candidate','control']):
  r=run([a,'shell','env',*(['NXT_SHA256_ACCELERATE=1'] if mode=='candidate' else []),base+mode,'bench']);x=json.loads(r.stdout);x.update(rep=rep,mode=mode);rows.append(x)
(p/'results.json').write_text(json.dumps(dict(checks=checks,runs=rows),indent=2)+'\n');print(json.dumps(dict(checks=checks,runs=rows),indent=2))
