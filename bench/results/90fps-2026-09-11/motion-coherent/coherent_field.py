"""One local median pass with current-colour and displacement compatibility."""
import numpy as np
def clean(cur,field):
 color=cur.reshape(64,8,64,8,3).mean(axis=(1,3));result=field.copy();changed=np.zeros((64,64),bool)
 for y in range(64):
  for x in range(64):
   y0,y1=max(0,y-1),min(64,y+2);x0,x1=max(0,x-1),min(64,x+2)
   f=field[y0:y1,x0:x1];c=color[y0:y1,x0:x1]
   ok=(np.linalg.norm(c-color[y,x],axis=-1)<=.12)&(np.linalg.norm((f-field[y,x])*512,axis=-1)<=8)
   if ok.sum()>=3:result[y,x]=np.median(f[ok],axis=0);changed[y,x]=True
 return result,changed
