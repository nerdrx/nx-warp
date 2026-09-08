from pathlib import Path
import argparse
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import ListedColormap
from matplotlib.patches import Patch
p=argparse.ArgumentParser();p.add_argument("--out",type=Path,required=True);p.add_argument("--before",type=Path);p.add_argument("--after",type=Path);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
y,x=np.indices((34,34));d=np.maximum.reduce([13-x,x-20,13-y,y-20,np.zeros_like(x)])
m=np.select([d==0,d<=2,d<4,d<8],[0,1,2,3],default=4)
labels=["Native centre: 512 × 512", "PLANAR: 4 px cells", "PLANAR: 8 px cells", "PLANAR: 16 px cells", "PLANAR: 32 px cells"]
colors=["#22b8a0","#a8dfd1","#f7d68a","#f5ac65","#dc785c"]
fig,ax=plt.subplots(figsize=(9,6));ax.imshow(m,cmap=ListedColormap(colors),vmin=0,vmax=4,extent=[0,2176,2176,0],interpolation="nearest");ax.set(xlabel="Eye image x (pixels)",ylabel="Eye image y (pixels)",title="Graduated centre: sampling policy per eye");ax.legend(handles=[Patch(color=c,label=l) for c,l in zip(colors,labels)],loc="upper left",bbox_to_anchor=(1.02,1));fig.tight_layout();fig.savefig(a.out/"sampling-policy.png",dpi=150,bbox_inches="tight");plt.close(fig)
if a.before and a.after:
    def eye(path):
        return np.fromfile(path,dtype=np.uint8,count=4352*2176).reshape(2176,4352)[:,:2176]
    old,new=eye(a.before),eye(a.after)
    fig,axs=plt.subplots(1,2,figsize=(12,6))
    for ax,im,title in zip(axs,[old,new],["Before: abrupt centre / periphery", "After: graduated PLANAR cell sizes"]):
        ax.imshow(im,cmap="gray",vmin=0,vmax=255,interpolation="nearest");ax.set_title(title);ax.set_axis_off()
    fig.suptitle("Decoded Pico luma — same rendered-camera frame (not headset capture)");fig.tight_layout();fig.savefig(a.out/"decoded-comparison.png",dpi=150);plt.close(fig)
