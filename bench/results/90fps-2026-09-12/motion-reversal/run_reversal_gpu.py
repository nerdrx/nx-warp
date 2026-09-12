from pathlib import Path
import subprocess,os,json,re
from PIL import Image,ImageDraw,ImageFont
import numpy as np
r=Path(__file__).resolve().parent;inputs=sorted((r/'reversal-blender/linear').glob('*.rgba'));out=r/'reversal-gpu';out.mkdir(exist_ok=True);rows=[]
env=os.environ.copy();env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'
for win in [8,4,2]:
 exe=r/'window-study'/f'w{win}'/'motion_gpu_truth';paths=[]
 for i in range(1,len(inputs)-2):
  d=out/f'w{win}'/f'{i-1:03d}';d.mkdir(parents=True,exist_ok=True)
  q=subprocess.run([str(exe),'2','0','0',str(inputs[i-1]),str(inputs[i]),str(inputs[i+2])],cwd=d,env=env,text=True,capture_output=True)
  (d/'raw.log').write_text(q.stdout+q.stderr)
  if q.returncode or 'Validation Error' in q.stdout+q.stderr:raise RuntimeError(str(d)+' '+q.stdout+q.stderr)
  paths.append(str(d/'field.f32'))
 subprocess.run([str(r/'motion_history_replay'),'64',str(out/f'w{win}'/'packed')]+paths,stdout=(out/f'w{win}'/'replay.log').open('w'),check=True)
 for j,i in enumerate(range(1,len(inputs)-2)):
  base=out/f'w{win}'/f'{j:03d}'
  for name,step,kind in [('cap',2/3,'raw'),('medium',4/3,'raw'),('history',4/3,'ema')]:
   d=base/name;d.mkdir(exist_ok=True);en=env.copy();en['NX_MOTION_FIELD_OVERRIDE']=str(out/f'w{win}'/'packed'/f'{j}-{kind}.f32')
   q=subprocess.run([str(exe),str(step),'0','0',str(inputs[i-1]),str(inputs[i]),str(inputs[i+2])],cwd=d,env=en,text=True,capture_output=True)
   (d/'run.log').write_text(q.stdout+q.stderr)
   if q.returncode or 'Validation Error' in q.stdout+q.stderr:raise RuntimeError(str(d)+' '+q.stdout+q.stderr)
   m=re.search(r'held_rmse=([\d.]+) warped_rmse=([\d.]+)',q.stdout+q.stderr)
   rows.append({'window':win,'frame':j,'source_index':i,'method':name,'held_rmse':float(m[1]),'rmse':float(m[2])})
 print('completed window',win,flush=True)
(out/'scores.json').write_text(json.dumps(rows,indent=2));print('done',len(rows),'predictions',flush=True)
