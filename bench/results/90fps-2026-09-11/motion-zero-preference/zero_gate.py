"""Single causal photometric gate; CPU diagnostic, unchanged GPU presentation."""
import numpy as np
def sample(a,x,y):
 h,w=a.shape[:2];x=np.clip(x,0,w-1);y=np.clip(y,0,h-1);ix=np.floor(x).astype(int);iy=np.floor(y).astype(int);fx=(x-ix)[...,None];fy=(y-iy)[...,None]
 return (a[iy,ix]*(1-fx)+a[iy,np.minimum(ix+1,w-1)]*fx)*(1-fy)+(a[np.minimum(iy+1,h-1),ix]*(1-fx)+a[np.minimum(iy+1,h-1),np.minimum(ix+1,w-1)]*fx)*fy
def gate(prev,cur,field):
 y,x=np.mgrid[:512,:512];dx=np.repeat(np.repeat(field[:,:,0]*512,8,0),8,1);dy=np.repeat(np.repeat(field[:,:,1]*512,8,0),8,1)
 shifted=sample(prev,x-dx,y-dy)
 def patches(e):return np.mean(e.reshape(64,8,64,8,3),axis=(1,3,4))
 zero=patches(np.abs(cur-prev));cost=patches(np.abs(cur-shifted));keep=(zero-cost)>np.maximum(.1*zero,1/255)
 valid=((x-dx>=0)&(x-dx<=511)&(y-dy>=0)&(y-dy<=511)).reshape(64,8,64,8).all(axis=(1,3))
 keep &= valid
 return field*keep[:,:,None],keep,zero,cost
