from pathlib import Path
import json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).parent
s=json.loads((p/'summary.json').read_text())['runs']
means={m:{k:sum(r['decoder']['means'][k] for r in s if r['mode']==m)/2 for k in ['pass_a_ms','pass_b_ms']} for m in [256,64]}
fig,ax=plt.subplots(figsize=(7,4),layout='constrained')
a=[means[m]['pass_a_ms'] for m in [256,64]];b=[means[m]['pass_b_ms'] for m in [256,64]]
ax.bar(['256 threads','64 threads'],a,label='Pass A',color='#a59cd7')
ax.bar(['256 threads','64 threads'],b,bottom=a,label='Pass B',color='#6353b0')
for i,(x,y) in enumerate(zip(a,b)):
 ax.text(i,x/2,f'{x:.2f}',ha='center',va='center');ax.text(i,x+y/2,f'{y:.2f}',ha='center',va='center',color='white')
ax.set_ylabel('GPU milliseconds (mean of run means)');ax.set_title('2688 per eye: decode stages, four 120-second trials');ax.legend();fig.savefig(p/'decode-stages.png',dpi=160)
