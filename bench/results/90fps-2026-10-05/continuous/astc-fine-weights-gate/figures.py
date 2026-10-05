from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
base=Path(__file__).resolve().parent
with (base/"comparison.ppm").open("rb") as f:
    assert f.readline()==b"P6\n"
    w,h=map(int,f.readline().split()); assert f.readline()==b"255\n"
    pixels=np.frombuffer(f.read(),dtype=np.uint8).reshape(h,w,3)
patterns=[]
patterns.append(np.repeat(np.arange(8)[None,:,None]*255/7,8,axis=0).repeat(3,axis=2).round().astype(np.uint8))
for split in (3,4,5):
    patterns.append(np.repeat(((np.arange(8)>=split)*255)[None,:,None],8,axis=0).repeat(3,axis=2).astype(np.uint8))
colors=np.array([[255,20,10],[10,255,20],[20,10,255],[255,240,10]],dtype=np.uint8)
y,x=np.indices((8,8));patterns.append(colors[(y>=4)*2+(x>=4)])
fig,axes=plt.subplots(5,4,figsize=(10,11),facecolor="#171322")
labels=["Gray ramp","Edge x=3","Edge x=4","Edge x=5","Colour quadrants"]
titles=["Synthetic source","5×5 Q8","6×6 Q4","8×8 Q2"]
for row in range(5):
    for col in range(4):
        ax=axes[row,col];panel=patterns[row] if col==0 else pixels[row*192:(row+1)*192,(col-1)*192:col*192]
        ax.imshow(panel,interpolation="nearest");ax.set_xticks([]);ax.set_yticks([])
        if row==0:ax.set_title(titles[col],color="white",fontsize=12)
        if col==0:ax.set_ylabel(labels[row],color="white",fontsize=11)
fig.suptitle("Same 16-byte ASTC blocks: finer grids trade smooth shading for edges",color="white",fontsize=14)
fig.text(.5,.01,"CPU reference decode • toy farthest-pair fitter • no live/Pico performance claim",ha="center",color="#c9c1d8",fontsize=10)
fig.tight_layout(rect=[0,.03,1,.96]);fig.savefig(base/"comparison.png",dpi=150,facecolor=fig.get_facecolor());plt.close(fig)
