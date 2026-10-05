from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent
with (p/"contact.ppm").open("rb") as f:
    assert f.readline()==b"P6\n"
    w,h=map(int,f.readline().split());assert f.readline()==b"255\n"
    im=np.frombuffer(f.read(),dtype=np.uint8).reshape(h,w,3)
fig,axes=plt.subplots(3,4,figsize=(11,8),facecolor="#171322")
for y,label in enumerate(["Moving vertical edge","Moving diagonal","Moving ramp"]):
    for x,title in enumerate(["Source","5×5 Q8 baseline","8×8 Q2 control","Per-block selector"]):
        ax=axes[y,x];ax.imshow(im[y*128:(y+1)*128,x*128:(x+1)*128],interpolation="nearest");ax.set_xticks([]);ax.set_yticks([])
        if y==0:ax.set_title(title,color="white",fontsize=11)
        if x==0:ax.set_ylabel(label,color="white",fontsize=11)
fig.suptitle("Selective binary weights: keep gradients, sharpen simple edges",color="white",fontsize=14)
fig.text(.5,.01,"Final frame of 16×16 translated toys • CPU oracle • simple fitter, not production quality",color="#cbc0da",ha="center",fontsize=10)
fig.tight_layout(rect=[0,.04,1,.94]);fig.savefig(p/"contact.png",dpi=150,facecolor=fig.get_facecolor());plt.close(fig)
