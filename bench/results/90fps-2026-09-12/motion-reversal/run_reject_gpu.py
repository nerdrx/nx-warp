from pathlib import Path
import subprocess, os, re, json
r=Path(__file__).resolve().parent; inp=sorted((r.parent/'reversal-blender'/'linear').glob('*.rgba')); out=r/'w8'; exe=r.parent/'window-study'/'w8'/'motion_gpu_truth'; env=os.environ.copy(); env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'; rows=[]
for j,i in enumerate(range(1,len(inp)-2)):
 d=out/f'{j:03d}'/'reject'; d.mkdir(exist_ok=True); e=env.copy(); e['NX_MOTION_FIELD_OVERRIDE']=str(r/'w8'/'reject-packed'/f'{j}-reject.f32')
 q=subprocess.run([str(exe),str(4/3),'0','0',str(inp[i-1]),str(inp[i]),str(inp[i+2])],cwd=d,env=e,text=True,capture_output=True); (d/'run.log').write_text(q.stdout+q.stderr)
 if q.returncode or 'Validation Error' in q.stdout+q.stderr: raise RuntimeError(f'{d}: {q.stdout[-500:]} {q.stderr[-500:]}')
 m=re.search(r'held_rmse=([\d.]+) warped_rmse=([\d.]+)',q.stdout+q.stderr); assert m; rows.append({'frame':j,'source_index':i,'method':'reject','window':8,'step':4/3,'held_rmse':float(m[1]),'rmse':float(m[2])})
(r/'reject-scores.json').write_text(json.dumps({'variant':'per-cell sign-reject EMA','source':'actual GPU warp','validation':'VK_LAYER_KHRONOS_validation','rows':rows},indent=2)+'\n'); print('done',len(rows))
