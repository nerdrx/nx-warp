from pathlib import Path
import subprocess,json,gzip,shutil
p=Path(__file__).resolve().parent;root=p.parent.parent;a='/home/nerdrx/.local/bin/adb';live=root/'nx-scratch/motion-live'
def call(args,log=None):
 r=subprocess.run(args,capture_output=True,text=True)
 if log:(p/log).write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(f'{args[0]} failed; see {log}: '+r.stderr[-400:])
 return r.stdout

def restart(*args):
 pid=(live/'server-current.pid').read_text().strip();return call(['python3',str(live/'restart_server.py'),pid,'--force-owned',*args])
results=[]
try:
 for i,wait in enumerate([4000,2000,2000,4000]):
  on=1
  label=f'priority-wait-{i}-{wait}';csv=p/(label+'.csv')
  restart('--timings',str(csv));call([a,'shell','setprop','debug.wivrn.nx.decode_priority',str(on)])
  call(['python3',str(live/'capture_live.py'),label,'1',str(wait),'60','0'],label+'-capture.log')
  restart('--no-timings')
  for kind in ['client.log','server.log','scene.log','status.json']:shutil.copy2(live/(label+'-'+kind),p/(label+'-'+kind))
  lat=json.loads(call(['python3',str(root/'nx-warp/bench/results/90fps-2026-09-09/live-latency/summarize_pipeline_latency.py'),str(csv),'--codec','nx']))
  telemetry=json.loads(call(['python3',str(live/'analyze_live.py'),str(p/(label+'-client.log'))]))
  expected=f'NX queue priorities: render 0 decode {on} / {on} (3 queues)';assert expected in (p/(label+'-client.log')).read_text(),expected
  assert lat['stages']['blit']['count']>2000,lat
  with csv.open('rb') as src,gzip.open(str(csv)+'.gz','wb') as dst:shutil.copyfileobj(src,dst)
  result=dict(run=i,priority=on,wait_us=wait,latency=lat,telemetry=telemetry);results.append(result);(p/'results.json').write_text(json.dumps(results,indent=2)+'\n');print({'run':i,'priority':on,'wait_us':wait,'selection':lat['stages']['blit']},flush=True)
finally:
 restart('--no-timings');call([a,'shell','setprop','debug.wivrn.nx.ready_wait_us','4000']);call([a,'shell','setprop','debug.wivrn.nx.decode_priority','0']);call([a,'shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2','org.meumeu.wivrn.nx.warp'])
