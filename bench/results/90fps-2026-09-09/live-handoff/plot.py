from pathlib import Path
import json,re
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent;r=json.loads((p/'results.json').read_text());rows=[]
for line in (p/'client.log').read_text().splitlines():
 m=re.search(r'\d\d-\d\d (\d\d:\d\d:\d\d\.\d+).*nxwarp\[0\] handoff frame \d+: decode_gpu [\d.]+ gap_gpu ([\d.]+)',line)
 if m:
  h,mi,s=map(float,m[1].split(':'));rows.append((h*3600+mi*60+s,float(m[2])))
x=sorted(v for t,v in rows if t>=rows[0][0]+10);assert len(x)==r['frames'];y=np.arange(1,len(x)+1)*100/len(x)
fig,ax=plt.subplots(figsize=(9,4.5),layout='constrained');ax.step(x,y,where='post',color='#6253b5');ax.set_xscale('log');ax.set_ylim(0,101);ax.set_xlabel('Decode-end → output-start gap, milliseconds (log scale)');ax.set_ylabel('Cumulative percentage of frames');ax.spines[['top','right']].set_visible(False)
for q,label in zip(r['stages']['gap']['p50_p95_p99_ms'],['p50','p95','p99']):ax.axvline(q,color='#21a49a',alpha=.4)
ax.text(.97,.15,'Median 0.0142 ms · p99 0.0241 ms\n20 / 3,796 gaps exceed 1 ms\nMaximum 6.4801 ms',ha='right',transform=ax.transAxes,bbox={'facecolor':'white','edgecolor':'#ddd'})
ax.set_title('Pico motion run: output handoff is usually very small\nDevice timestamps; not encode-to-selection or photon latency');fig.savefig(p/'handoff.png',dpi=160)
