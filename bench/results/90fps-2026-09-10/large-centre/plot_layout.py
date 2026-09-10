from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import ListedColormap
p=Path(__file__).parent
fig,axs=plt.subplots(1,2,figsize=(10,5),constrained_layout=True)
for ax,c,label in zip(axs,[640,1024],['Previous centre','Larger centre option']):
 x=(np.arange(42)+.5)*64-1344
 rr=np.hypot(x[:,None],x[None,:])/(c/2)
 d=np.ceil(np.maximum(0,rr*.886226925452758-1)*(c/128))
 mask=np.where(rr<=1,2,np.where(d<=4,1,0))
 ax.imshow(mask,cmap=ListedColormap(['#465268','#59b2b7','#e7f9ed']),origin='lower',extent=[0,2688,0,2688],vmin=0,vmax=2)
 ax.set_title(f'{label}: {c}px diameter\n{int((mask==2).sum())} native tiles per eye')
 ax.set_xlabel('Eye pixels');ax.set_ylabel('Eye pixels')
fig.suptitle('2688² per eye — actual encoder tile masks\nLight: native centre · teal: fine PLANAR · dark: coarse PLANAR',fontsize=12)
fig.savefig(p/'centre-layout.png',dpi=150)
