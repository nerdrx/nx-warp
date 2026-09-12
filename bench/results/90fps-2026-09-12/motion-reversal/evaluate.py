"""Compact evaluation of actual GPU reversal outputs."""
from pathlib import Path
import json, csv, subprocess, hashlib
import numpy as np
from PIL import Image, ImageDraw
R=Path(__file__).resolve().parent.parent; OUT=R/'report'; OUT.mkdir(exist_ok=True)
def ppm(p): return np.asarray(Image.open(p).convert('RGB'),np.float32)
def centroid(a):
    r,g,b=a[...,0],a[...,1],a[...,2]; m=(g>1.4*r)&(g>1.15*b)&(g>35); yy,xx=np.where(m)
    return np.array([xx.mean(),yy.mean()]) if len(xx)>=100 else np.array([np.nan,np.nan])
scores=json.loads((R/'scores.json').read_text())+json.loads((R/'reject-scores.json').read_text())['rows']; rows=[]
def pred(d,m): return d/'held.ppm' if m=='held' else d/m/'warped.ppm'
def rms2(q):
    z=np.diff(q,n=2,axis=0); z=z[np.isfinite(z).all(1)]
    return float(np.sqrt(np.mean(np.sum(z*z,axis=1)))) if len(z) else None
per=[]
traj={int(x['frame']):x for x in csv.DictReader((R.parent/'reversal-blender'/'trajectory.csv').open())}
for w in (8,4,2):
    for method in (('cap','medium','history','reject') if w==8 else ('cap','medium','history')):
        ss=[x for x in scores if x['window']==w and x['method']==method]; errs=[]; residual=[]
        for x in sorted(ss,key=lambda z:z['frame']):
            d=R/f'w{w}'/f"{x['frame']:03d}"; truth=ppm(d/'truth.ppm'); img=ppm(pred(d,method)); errs.append(float(x['rmse'])); residual.append(centroid(img)-centroid(truth))
            if w==8: per.append({'frame':x['frame'],'method':method,'phase':traj[x['source_index']+1]['phase'],'rmse':errs[-1],'centroid_dx':float(residual[-1][0]),'centroid_dy':float(residual[-1][1])})
        q=np.asarray(residual); valid=q[np.isfinite(q).all(1)]
        rows.append({'window':w,'method':method,'frames':len(ss),'mean_rmse':float(np.mean(errs)),'mean_centroid_residual_px':float(np.mean(np.linalg.norm(valid,axis=1))) if len(valid) else None,'mean_centroid_second_difference_rms_px':rms2(q),'valid_centroids':int(len(valid))})
ss=sorted([x for x in scores if x['window']==8 and x['method']=='cap'],key=lambda z:z['frame']); errs=[]; q=[]
for x in ss:
    d=R/'w8'/f"{x['frame']:03d}"; t=ppm(d/'truth.ppm'); h=ppm(d/'held.ppm'); errs.append(x['held_rmse']); q.append(centroid(h)-centroid(t))
q=np.asarray(q); valid=q[np.isfinite(q).all(1)]; rows.insert(0,{'window':8,'method':'held','frames':len(ss),'mean_rmse':float(np.mean(errs)),'mean_centroid_residual_px':float(np.mean(np.linalg.norm(valid,axis=1))),'mean_centroid_second_difference_rms_px':rms2(q),'valid_centroids':len(valid)})
# Project predicted displacement onto held-to-future truth displacement.
truthc=[]; heldc=[]
for x in ss:
    d=R/'w8'/f"{x['frame']:03d}"
    truthc.append(centroid(ppm(d/'truth.ppm')))
    heldc.append(centroid(ppm(d/'held.ppm')))
truthc=np.asarray(truthc); heldc=np.asarray(heldc)
delta=truthc-heldc; denom=np.sum(delta*delta,axis=1)
eligible=np.isfinite(denom)&(denom>=16)
phases=np.array([traj[x['source_index']+1]['phase'] for x in ss])
for row in rows:
    if row['window']!=8: continue
    vals=np.asarray([centroid(ppm(pred(R/'w8'/f"{x['frame']:03d}",row['method']))) for x in ss])
    ok=eligible&np.isfinite(vals).all(1)
    row['mean_projected_progress']=float(np.mean(np.sum((vals[ok]-heldc[ok])*delta[ok],axis=1)/denom[ok]))
    row['eligible_frames_ge4px']=int(ok.sum())
    q=vals-truthc
    row['phases']={}
    for phase in ('forward','stop','reverse'):
        mask=phases==phase; valid=mask&np.isfinite(q).all(1)
        # Only count temporal triples wholly contained in this phase.
        z=np.diff(q,n=2,axis=0); triples=mask[:-2]&mask[1:-1]&mask[2:]&np.isfinite(z).all(1)
        row['phases'][phase]={'frames':int(mask.sum()),'mean_centroid_error_px':float(np.mean(np.linalg.norm(q[valid],axis=1))),
            'jitter_proxy_px':float(np.sqrt(np.mean(np.sum(z[triples]**2,axis=1)))) if triples.any() else None}
(OUT/'metrics.json').write_text(json.dumps({'fixture':'reversal-blender','host_resolution':'512x512','source':'actual GPU PPM outputs','mask_threshold':'G>1.4R, G>1.15B, G>35, area>=100','trajectory_phase_frames':{p:sum(v['phase']==p for v in traj.values()) for p in ('forward','stop','reverse')},'second_difference_formula':'sqrt(mean(sum(vector_second_difference_px^2, axis=1))) after chronological finite-difference validity mask','rows':rows},indent=2)+'\n')
with (OUT/'metrics.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=sorted({k for row in rows for k in row if k!='phases'})); w.writeheader(); w.writerows([{k:v for k,v in row.items() if k!='phases'} for row in rows])
(OUT/'per_frame.csv').write_text('')
with (OUT/'per_frame.csv').open('w',newline='') as f: w=csv.DictWriter(f,fieldnames=per[0]); w.writeheader(); w.writerows(per)
assert len({hashlib.sha256(pred(R/'w8'/'027',m).read_bytes()).hexdigest() for m in ('cap','medium','history')})>1
from PIL import ImageFont
font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',20)
def display_image(path):
    return Image.open(path).convert('RGB')
def animate(labels,columns,prefix):
    height=552; nr=(len(labels)+columns-1)//columns
    for f in range(57):
        c=Image.new('RGB',(columns*512,nr*height),(12,17,29)); d=ImageDraw.Draw(c)
        for k,(label,w,m) in enumerate(labels):
            base=R/f'w{w}'/f'{f:03d}'
            p=base/'truth.ppm' if m=='truth' else pred(base,m)
            xx=(k%columns)*512; yy=(k//columns)*height
            c.paste(display_image(p),(xx,yy+40)); d.text((xx+8,yy+8),label,fill='white',font=font)
        if prefix=='comparison' and len(labels)<6:
            xx=2*512+16; yy=552+55
            for j,line in enumerate(['Offline GPU fixture', '512 x 512 per eye', '60 Hz source; 15 fps slow replay', f'Source frame {f+2}; truth frame {f+4}', '', '8px vector grid, 32px matching patch', 'No blur. Future is reference only.', 'Shader outputs shown directly (sRGB).', '', 'Centroid scores are one-object proxies.', 'No headset latency measurement here.']):
                d.text((xx,yy+j*34),line,fill='#b9c9e5',font=font)
        c.save(OUT/f'{prefix}_{f:03d}.png')
    subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(OUT/f'{prefix}_%03d.png'),'-c:v','libx264','-crf','18','-pix_fmt','yuv420p',str(OUT/f'{prefix}.mp4')],check=True)
animate([('Future truth (+33.33 ms)',8,'truth'),('Held source',8,'held'),('Raw cap: 11.11 ms',8,'cap'),('Raw stronger: 22.22 ms',8,'medium'),('History: 22.22 ms',8,'history'),('Direction reset: 22.22 ms',8,'reject')],3,'comparison')
animate([('History: 32px matching patch',8,'history'),('History: 16px matching patch',4,'history'),('History: 8px matching patch',2,'history')],3,'history_windows')
print(json.dumps(rows,indent=2))
