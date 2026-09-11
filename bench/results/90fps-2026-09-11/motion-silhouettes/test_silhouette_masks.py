import numpy as np
from silhouette_masks import refine_masks

def main():
    m=np.zeros((64,64),np.uint8); m[10:45,10:45]=1; m[20:24,20:24]=0
    out,r=refine_masks([m]); assert out[0][21,21] and r[0]['holes_filled']>0
    # A deep concavity must remain; no convex hull is allowed.
    c=np.zeros((64,64),np.uint8); c[10:50,10:20]=1; c[40:50,10:45]=1
    out,r=refine_masks([c]); assert not out[0][20,30]
    a=np.zeros((64,64),np.uint8); b=np.zeros_like(a); a[10:30,10:30]=1; b[10:30,30:50]=1
    out,r=refine_masks([a,b]); assert not np.any(out[0]&out[1])
    # A hole touching no border but larger than the limit remains open.
    big=np.zeros((64,64),np.uint8); big[5:55,5:55]=1; big[20:40,20:40]=0
    out,r=refine_masks([big],max_hole=64); assert not out[0][25,25]
    out,r=refine_masks([m],max_hole=0); assert not out[0][21,21]
    # Exterior flood fill must work even when the foreground touches (0,0).
    corner=np.ones((20,20),np.uint8); corner[8:10,8:10]=0
    out,r=refine_masks([corner]); assert out[0][9,9]
    exterior=np.ones((64,64),np.uint8); exterior[:3,30:33]=0
    out,r=refine_masks([exterior],epsilon=0); assert not out[0][1,31] and r[0]['holes_filled']==0
    print('PASS: hole fill, concavity preservation, neighbouring masks')
if __name__=='__main__': main()
