"""Image-only layered predictor using hue components and region moments."""
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent/'python-deps'))
import json, subprocess, cv2, numpy as np
from PIL import Image, ImageDraw, ImageFont

r=Path(__file__).resolve().parent; s=r.parent/'motion-scene'; out=r/'layered-moments'; (out/'frames').mkdir(parents=True,exist_ok=True)
cv2.setNumThreads(4); font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',19)

def comps(im):
    hsv=cv2.cvtColor(im,cv2.COLOR_RGB2HSV); h,sat,val=[hsv[:,:,i].astype(float) for i in range(3)]; allc=[]
    for centre in range(0,180,15):
        dh=np.minimum(abs(h-centre),180-abs(h-centre)); m=((dh<12)&(sat>75)&(val>35)).astype(np.uint8)
        m=cv2.morphologyEx(m,cv2.MORPH_CLOSE,np.ones((3,3),np.uint8)); n,lab,st,_=cv2.connectedComponentsWithStats(m)
        for k in range(1,n):
            area=int(st[k,cv2.CC_STAT_AREA]); q=(lab==k).astype(np.uint8)
            if not 180<area<60000 or any(np.count_nonzero(q&z[0])/min(area,int(z[0].sum()))>.6 for z in allc): continue
            ys,xs=np.where(q); pts=np.stack((xs,ys),1).astype(float); c=pts.mean(0); cov=np.cov(pts.T) if len(pts)>2 else np.eye(2)
            ev,vec=np.linalg.eigh(cov); order=np.argsort(ev)[::-1]
            allc.append((q,c,ev[order],vec[:,order[0]],float(h[ys,xs].mean())))
    return allc

def model(old,new):
    # Fit the forward old->new similarity, then explicitly invert it for the
    # current->previous sampling map used by the warp.
    ratio=np.sqrt(max(new[2].sum(),1e-6)/max(old[2].sum(),1e-6)); ratio=float(np.clip(ratio,.85,1.18)); angle=0.0
    if new[2][0]>2.5*max(new[2][1],1e-5) and old[2][0]>2.5*max(old[2][1],1e-5):
        a=np.arctan2(new[3][1],new[3][0]); b=np.arctan2(old[3][1],old[3][0]); angle=float(np.arctan2(np.sin(a-b),np.cos(a-b)))
        # PCA axes are unoriented: identify angles modulo pi, then keep a
        # conservative signed rotation.
        angle=float(np.clip(np.arctan2(np.sin(2*angle),np.cos(2*angle))/2,-np.pi/3,np.pi/3))
    c,si=np.cos(angle),np.sin(angle); F=ratio*np.array([[c,-si],[si,c]]); A=np.linalg.inv(F)
    # current point p maps to previous q=A(p-new_c)+old_c; H maps source to dest.
    H=np.array([[A[0,0],A[0,1],old[1][0]-A[0]@new[1]],[A[1,0],A[1,1],old[1][1]-A[1]@new[1]]],np.float32)
    return H

def _axis_delta(a,b):
    d=np.arctan2(np.sin(a-b),np.cos(a-b))
    return np.arctan2(np.sin(2*d),np.cos(2*d))/2

assert abs(_axis_delta(np.deg2rad(10),np.deg2rad(190))-np.deg2rad(-0.0)) < 1e-9

rows=[]
for idx,i in enumerate(range(2,34)):
    prev=np.array(Image.open(s/'frames'/f'frame_{i-1:04d}.png').convert('RGB')); cur=np.array(Image.open(s/'frames'/f'frame_{i+1:04d}.png').convert('RGB'))
    old,new=comps(prev),comps(cur); used=set(); objects=[]
    for n in new:
        cand=[]
        for j,o in enumerate(old):
            if j in used: continue
            dh=min(abs(n[4]-o[4]),180-abs(n[4]-o[4])); da=abs(np.log((n[0].sum()+1)/(o[0].sum()+1))); dp=np.linalg.norm(n[1]-o[1])/512
            cand.append((dh/30+da*.15+dp,j,o))
        if not cand: continue
        score,j,o=min(cand)
        if score>.9 or np.linalg.norm(n[1]-o[1])>130: continue
        used.add(j); objects.append((n[0],model(o,n),1-score))
    union=np.zeros((512,512),np.uint8)
    for m,H,c in objects: union|=m
    pred=cv2.inpaint(cur,union*255,3,cv2.INPAINT_TELEA)
    for m,H,c in sorted(objects,key=lambda z:-int(z[0].sum())):
        M=cv2.invertAffineTransform(H); col=cv2.warpAffine(cur,M,(512,512),flags=cv2.INTER_LINEAR); al=cv2.warpAffine(m.astype(np.float32),M,(512,512),flags=cv2.INTER_LINEAR)
        pred=np.uint8(np.clip(pred*(1-al[:,:,None])+col*al[:,:,None],0,255))
    truth=np.array(Image.open(s/'frames'/f'frame_{i+3:04d}.png').convert('RGB')); crop=np.s_[64:-64,64:-64,:]
    rm=lambda a:float(np.sqrt(np.mean((a[crop].astype(float)-truth[crop])**2)))
    rows.append(dict(frame=idx,objects=len(objects),rmse=rm(pred),held_rmse=rm(cur)))
    im=Image.new('RGB',(1536,552),'#101725'); dr=ImageDraw.Draw(im)
    for k,(a,t) in enumerate(((cur,'Held current'),(pred,'Moment regions'),(truth,'Correct future'))): im.paste(Image.fromarray(a),(512*k,40)); dr.text((512*k+8,8),t,font=font,fill='white')
    im.save(out/'frames'/f'{idx:03d}.png'); print(idx,len(objects),round(rows[-1]['rmse'],2),flush=True)
(out/'scores.json').write_text(json.dumps(rows,indent=2)); subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(out/'frames/%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
ims=[Image.open(p).resize((960,345)) for p in sorted((out/'frames').glob('*.png'))[::2]]; ims[0].save(out/'comparison.gif',save_all=True,append_images=ims[1:],duration=133,loop=0)
print('means', {k:float(np.mean([v[k] for v in rows])) for k in ('rmse','held_rmse','objects')})
