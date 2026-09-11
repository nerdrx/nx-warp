"""Causal RGB background cache; CPU diagnostic with feature-based alignment."""
import cv2,numpy as np
class Background:
 def __init__(self):self.rgb=None;self.known=None;self.gray=None;self.visible=None;self.inliers=0.
 def update(self,cur,tracks):
  h,w=cur.shape[:2];occupied=np.zeros((h,w),np.uint8)
  for t in tracks:occupied|=t['mask']
  visible=1-cv2.dilate(occupied,np.ones((7,7),np.uint8));gray=cv2.cvtColor(cur,cv2.COLOR_RGB2GRAY)
  if self.rgb is None:
   self.rgb=cur.copy();self.known=visible.copy()
  else:
   p=cv2.goodFeaturesToTrack(self.gray,300,.02,6,mask=self.visible*255)
   H=None;self.inliers=0.
   if p is not None and len(p)>=12:
    q,status,_=cv2.calcOpticalFlowPyrLK(self.gray,gray,p,None,winSize=(15,15),maxLevel=2)
    if q is not None:
     a=p.reshape(-1,2);b=q.reshape(-1,2);xy=np.clip(np.rint(b),[0,0],[w-1,h-1]).astype(int)
     ok=status.ravel().astype(bool)&visible[xy[:,1],xy[:,0]].astype(bool)&np.isfinite(b).all(1)
     if ok.sum()>=12:
      H,ins=cv2.estimateAffinePartial2D(a[ok],b[ok],method=cv2.RANSAC,ransacReprojThreshold=1.5)
      self.inliers=float(ins.mean()) if ins is not None else 0.
   if H is None or self.inliers<.6 or not .95<np.linalg.norm(H[0,:2])<1.05 or np.linalg.norm(H[:,2])>30:
    # Invalid alignment discards history instead of reusing stale coordinates.
    self.rgb=cur.copy();self.known=visible.copy()
   else:
    self.rgb=cv2.warpAffine(self.rgb,H,(w,h));self.known=cv2.warpAffine(self.known,H,(w,h),flags=cv2.INTER_NEAREST)
    self.rgb[visible.astype(bool)]=cur[visible.astype(bool)];self.known|=visible
  self.gray=gray;self.visible=visible
 def fill(self,cur,holes):
  valid=holes.astype(bool)&self.known.astype(bool);pred=cur.copy();pred[valid]=self.rgb[valid]
  missing=holes.copy();missing[valid]=0
  if missing.any():pred=cv2.inpaint(pred,missing*255,3,cv2.INPAINT_TELEA)
  return pred,int(valid.sum())
