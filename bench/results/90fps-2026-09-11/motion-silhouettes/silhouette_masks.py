"""Conservative image-only cleanup for already disjoint region masks."""
from pathlib import Path
import sys
try: import cv2
except ImportError:
    sys.path.insert(0,str(Path(__file__).resolve().parent/'python-deps')); import cv2
import numpy as np

def _fill_small_holes(m,max_hole=64):
    padded=np.pad((~m).astype(np.uint8),1,constant_values=1); ff=np.zeros((padded.shape[0]+2,padded.shape[1]+2),np.uint8)
    cv2.floodFill(padded,ff,(0,0),2); holes=padded[1:-1,1:-1].astype(bool)&(padded[1:-1,1:-1]!=2)
    n,lab,st,_=cv2.connectedComponentsWithStats(holes.astype(np.uint8),8)
    out=m.copy(); count=0
    for i in range(1,n):
        if st[i,cv2.CC_STAT_AREA]<=max_hole: out[lab==i]=1; count+=1
    return out,count,holes & ~out

def refine_masks(masks, epsilon=1.0, max_hole=64):
    """Return refined uint8 masks and per-mask accepted-change counts."""
    src=[np.asarray(m,dtype=np.uint8).astype(bool) for m in masks]; allsrc=np.zeros_like(src[0],bool) if src else np.zeros((0,0),bool)
    for m in src: allsrc|=m
    out=[]; report=[]; occupied=allsrc.copy()
    for m in src:
        base,holes,large_holes=_fill_small_holes(m,max_hole)
        contours,_=cv2.findContours(base.astype(np.uint8),cv2.RETR_EXTERNAL,cv2.CHAIN_APPROX_SIMPLE)
        simp=np.zeros_like(base,np.uint8)
        for c in contours: cv2.fillPoly(simp,[cv2.approxPolyDP(c,epsilon,True)],1)
        candidate=simp.astype(bool) & ~large_holes; new=candidate & ~m; other=occupied & ~m
        dist=cv2.distanceTransform((~m).astype(np.uint8),cv2.DIST_L2,3)
        area_delta=abs(int(candidate.sum())-int(m.sum()))/max(1,int(m.sum()))
        accepted=area_delta<=.03 and not np.any(candidate&other) and (not np.any(new) or float(dist[new].max())<=2.01)
        chosen=candidate if accepted else m.astype(bool); out.append(chosen.astype(np.uint8)); occupied|=chosen
        report.append({'accepted':bool(accepted),'holes_filled':holes if accepted else 0,'area_change':area_delta if accepted else 0,'new_pixels':int(new.sum()) if accepted else 0})
    return out,report
