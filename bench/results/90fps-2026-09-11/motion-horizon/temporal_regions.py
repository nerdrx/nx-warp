"""Small, conservative image-only colour-region tracker."""
from pathlib import Path
import sys
try:
    import cv2
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parent/'python-deps')); import cv2
import numpy as np

def _merge_adjacent(regs, rgb, hue_limit=20., rgb_limit=42.):
    changed=True
    while changed:
        changed=False
        for i in range(len(regs)):
            for j in range(i+1,len(regs)):
                a,b=regs[i],regs[j]; ma,mb=a['mask'],b['mask']
                contact=int(np.count_nonzero(ma[:,1:]&mb[:,:-1])+np.count_nonzero(ma[:,:-1]&mb[:,1:])+np.count_nonzero(ma[1:]&mb[:-1])+np.count_nonzero(ma[:-1]&mb[1:]))
                dh=min(abs(a['hue']-b['hue']),180-abs(a['hue']-b['hue']))
                if contact<3 or dh>hue_limit or np.linalg.norm(rgb[ma].mean(0)-rgb[mb].mean(0))>rgb_limit: continue
                m=ma|mb; ys,xs=np.where(m); box=(xs.max()-xs.min()+1)*(ys.max()-ys.min()+1)
                if m.sum()>0.25*rgb.shape[0]*rgb.shape[1] or box>4*m.sum(): continue
                xy=np.stack((xs,ys),1).astype(float); c=xy.mean(0); z=xy-c; cov=(z.T@z)/max(1,len(xy)-1); ev=np.linalg.eigvalsh(cov)[::-1]
                hh=cv2.cvtColor(rgb.astype(np.uint8),cv2.COLOR_RGB2HSV)[:,:,0][m].astype(float)*np.pi/90
                hue=float((np.arctan2(np.sin(hh).mean(),np.cos(hh).mean())*90/np.pi)%180)
                regs[i]={'mask':m,'centroid':c,'area':int(m.sum()),'hue':hue,'cov':cov,'eig':ev}; regs.pop(j); changed=True; break
            if changed: break
    return regs

def segment_regions(rgb, hue_bins=24, min_area=24, close_px=2, merge_adjacent=False):
    """Return disjoint saturated colour components with masks and moments."""
    a=np.asarray(rgb); hsv=cv2.cvtColor(a.astype(np.uint8),cv2.COLOR_RGB2HSV); h,s,v=[hsv[:,:,i] for i in range(3)]
    # Quantisation gives each pixel exactly one hue class; no overlapping bands.
    q=((h.astype(np.int32)*hue_bins+90)//180)%hue_bins; eligible=(s>=70)&(v>=35); out=[]
    kernel=np.ones((2*close_px+1,2*close_px+1),np.uint8)
    occupied=np.zeros((a.shape[0],a.shape[1]),bool)
    for k in range(hue_bins):
        band=(eligible&(q==k)).astype(np.uint8)
        if close_px: band=cv2.morphologyEx(band,cv2.MORPH_CLOSE,kernel)
        band[occupied]=0
        n,lab,st,cent=cv2.connectedComponentsWithStats(band,8)
        for j in range(1,n):
            area=int(st[j,cv2.CC_STAT_AREA])
            if area<min_area: continue
            m=lab==j; ys,xs=np.where(m); xy=np.stack((xs,ys),1).astype(float); c=xy.mean(0); z=xy-c
            cov=(z.T@z)/max(1,len(xy)-1); ev=np.linalg.eigvalsh(cov)[::-1]
            ang=h[m].astype(float)*np.pi/90.0; hue=float((np.arctan2(np.sin(ang).mean(),np.cos(ang).mean())*90/np.pi)%180)
            occupied[m]=1; out.append({'mask':m,'centroid':c,'area':area,'hue':hue,'cov':cov,'eig':ev})
    return _merge_adjacent(out,a) if merge_adjacent else out

class TemporalRegionTracker:
    def __init__(self, max_jump=120., area_ratio=.45, velocity_alpha=.45, hue_bins=24, min_area=24, merge_adjacent=False):
        self.hue_bins=hue_bins; self.min_area=min_area; self.merge_adjacent=merge_adjacent; self.max_jump=max_jump; self.area_ratio=area_ratio; self.alpha=velocity_alpha; self.next_id=0; self.state={}

    def update(self, rgb, dt=1.):
        regs=segment_regions(rgb,hue_bins=self.hue_bins,min_area=self.min_area,merge_adjacent=self.merge_adjacent); candidates=[]
        pairs=[]
        for ri,r in enumerate(regs):
            for ident,s in self.state.items():
                dh=min(abs(r['hue']-s['hue']),180-abs(r['hue']-s['hue']))
                pred=s['centroid']+s['velocity']*dt; jump=np.linalg.norm(r['centroid']-pred)
                ar=min(r['area'],s['area'])/max(r['area'],s['area']); shape=min(r['eig'].sum(),s['eig'].sum())/max(r['eig'].sum(),s['eig'].sum(),1e-6)
                score=dh/35+jump/self.max_jump+(1-ar)+(1-shape)
                if dh<28 and jump<=self.max_jump and ar>=self.area_ratio and shape>=.45: pairs.append((score,ri,ident,s))
        assigned={}; taken=set()
        for score,ri,ident,s in sorted(pairs):
            if ri not in assigned and ident not in taken: assigned[ri]=(score,ident,s); taken.add(ident)
        candidates=[(assigned.get(ri),r) for ri,r in enumerate(regs)]
        used=set(); result=[]; newstate={}
        for best,r in sorted(candidates,key=lambda x:x[1]['area'],reverse=True):
            if best is None or best[1] in used: ident=self.next_id; self.next_id+=1; vel=np.zeros(2); age=1; conf=.25
            else:
                score,ident,s=best; used.add(ident); raw=(r['centroid']-s['centroid'])/max(dt,1e-6); vel=(1-self.alpha)*s['velocity']+self.alpha*raw; age=s['age']+1; conf=float(np.exp(-score))
            newstate[ident]={'centroid':r['centroid'],'velocity':vel,'area':r['area'],'hue':r['hue'],'eig':r['eig'],'age':age}
            confirmed=age>=3
            result.append({'id':int(ident),'mask':r['mask'].astype(np.uint8),'centroid':r['centroid'].copy(),
                           'velocity':vel.copy() if confirmed else np.zeros(2),
                           'confidence':conf if confirmed else 0.0,'matches':age-1})
        self.state=newstate
        return result
