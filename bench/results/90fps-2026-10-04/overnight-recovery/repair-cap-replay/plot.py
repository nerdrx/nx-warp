from pathlib import Path
import csv
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).parent
rows=list(csv.DictReader((p/'results.csv').open()))
losses=[8,32,64,96,128,192,256];caps=[64,128,256]
fig,axs=plt.subplots(1,2,figsize=(10,5),layout='constrained')
for ax,parity in zip(axs,[1,0]):
 data=np.empty((len(losses),len(caps))); labels={}
 for i,l in enumerate(losses):
  for j,c in enumerate(caps):
   r=next(r for r in rows if int(r['shards'])==521 and int(r['k'])==16 and int(r['parity'])==parity and int(r['loss'])==l and int(r['cap'])==c)
   left=int(r['remaining']);rounds=int(r['rounds'])
   data[i,j]=int(left!=0)
   labels[i,j]=f'{left} missing' if left else f'Complete\n{rounds} round'+('s' if rounds!=1 else '')
 ax.imshow(data,vmin=0,vmax=1,cmap=matplotlib.colors.ListedColormap(['#d8f0de','#f7c1bb']),aspect='auto')
 for (i,j),txt in labels.items():ax.text(j,i,txt,ha='center',va='center',fontsize=9)
 ax.set_xticks(range(3),['64 (current)','128 (trial)','256 (trial)'])
 ax.set_yticks(range(7),losses);ax.set_ylabel('Consecutive data shards lost');ax.set_xlabel('Maximum replies per request')
 ax.set_title('All parity retained' if parity else 'No parity retained')
fig.suptitle('521-shard frame: isolated repair ceiling after at most two rounds',fontsize=14)
fig.text(.5,-.02,'Production FEC k=16, interleave=4; ideal repair arrivals. No network timing or shared-budget proof.',ha='center',fontsize=9)
fig.savefig(p/'repair-ceiling.png',dpi=180,bbox_inches='tight')
