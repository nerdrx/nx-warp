"""Illustrate detail cadence, not measured timing."""
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import ListedColormap
from matplotlib.patches import Patch
f=np.arange(1,9);a=np.vstack([f%2==0,f%2==0,f%2==0,f%2==1])
fig,ax=plt.subplots(figsize=(10,3.8))
ax.imshow(a,cmap=ListedColormap(['#d4e6f4','#7950d1']),vmin=0,vmax=1,aspect='auto')
for y in range(4):
 for x in range(8):ax.text(x,y,'Detail' if a[y,x] else 'Guide + history',ha='center',va='center',fontsize=8,color='white' if a[y,x] else '#152336')
ax.set_yticks(range(4),['Sync · left','Sync · right','Alternating · left','Alternating · right'])
ax.set_xticks(range(8),[str(x) for x in f]);ax.set_xlabel('Admitted source frame (not display refresh)')
ax.axhline(1.5,color='white',lw=5)
ax.set_title('Both native centres stay fresh on every frame',pad=12)
fig.text(.5,.01,'Same detail budget per two-frame pair. Alternating spreads peak work but introduces unequal eye-detail age.',ha='center',fontsize=9)
fig.tight_layout(rect=[0,.06,1,1]);fig.savefig(Path(__file__).with_name('schedule.png'),dpi=150)
