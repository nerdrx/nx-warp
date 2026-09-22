import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from pathlib import Path
p=Path(__file__).parent
x,y=np.meshgrid(np.arange(128),np.arange(128));r=np.hypot(x-63.5,y-63.5)
t=np.clip((r-32)/31,0,1);w=1-t**3*(t*(t*6-15)+10)
fig,ax=plt.subplots(1,2,figsize=(10,4),layout="constrained")
ax[0].imshow(w,cmap="magma",vmin=0,vmax=1);ax[0].set_title("Native colour contribution — circular")
ax[0].set_xlabel("Encoded pixels");ax[0].set_ylabel("Encoded pixels")
r=np.linspace(0,90,400);t=np.clip((r-32)/31,0,1)
ax[1].plot(r,1-t**3*(t*(t*6-15)+10),color="#7700ff",linewidth=3)
ax[1].set(xlabel="Distance from centre (pixels)",ylabel="Native contribution",title="Continuous fade; no quality bands")
ax[1].axvline(32,color="gray",linestyle=":");ax[1].axvline(63,color="gray",linestyle=":");ax[1].grid(alpha=.2)
fig.suptitle("64 px native diameter + 31 px smooth transition per side")
fig.savefig(p/"falloff.png",dpi=160)
