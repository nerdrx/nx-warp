import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from pathlib import Path
p=Path(__file__).parent
fig,ax=plt.subplots(1,2,figsize=(11,4),layout="constrained")
r=np.linspace(0,135,600)
for rate in [100,200,350,500,700]:
 detail=rate-min(20,rate/4)
 radius=127*np.sqrt(np.clip((detail-80)/600,0,1))
 core=radius*64/127
 if radius:
  t=np.clip((r-core)/(radius-core),0,1);a=np.clip(radius/16,0,1);w=(1-t**3*(t*(t*6-15)+10))*a*a*(3-2*a)
 else:w=r*0
 ax[0].plot(r,w,label=f"{rate} Mbit/s")
rate=np.linspace(1,700,700);detail=rate-np.minimum(20,rate/4)
radius=127*np.sqrt(np.clip((detail-80)/600,0,1))
ax[1].plot(rate,2*radius*64/127,label="Sharp core diameter",color="#7700ff")
ax[1].plot(rate,2*radius,label="Including fade",color="#00a6bb")
ax[0].set(xlabel="Radius from centre (encoded pixels)",ylabel="Native-colour contribution",title="Continuous fade at each bitrate")
ax[1].set(xlabel="Total target bitrate (Mbit/s)",ylabel="Diameter (encoded pixels)",title="90 Hz budget policy — not measured throughput")
for a in ax:a.grid(alpha=.2);a.legend()
fig.savefig(p/"adaptive-falloff.png",dpi=150)
