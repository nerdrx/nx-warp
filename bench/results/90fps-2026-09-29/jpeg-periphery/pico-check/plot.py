"""Scientific figure from the published standalone Pico timing CSV."""
import csv
import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).parent
rows = list(csv.DictReader((ROOT/'pico-summary.csv').open()))
rows.sort(key=lambda r: (r['scene'],r['pixel_format']))
labels = [f"{r['scene'].title()} · {r['pixel_format']}" for r in rows]
p50 = [float(r['decode_p50_us'])/1000 for r in rows]
p95 = [float(r['decode_p95_us'])/1000 for r in rows]
colors = ['#58d6d0','#ab8ef5','#58d6d0','#ab8ef5']

fig, ax = plt.subplots(figsize=(10,4.6),facecolor='#100c20')
ax.set_facecolor('#19122b')
y = list(range(4))
ax.barh(y,p50,height=.54,color=colors,zorder=3)
ax.errorbar(p50,y,xerr=[[0]*4,[a-b for a,b in zip(p95,p50)]],
            fmt='none',ecolor='white',capsize=4,zorder=4)
for i,(med,tail) in enumerate(zip(p50,p95)):
    ax.text(tail+.04,i,f'{med:.3f} / {tail:.3f} ms',va='center',color='#eee9ff',fontsize=10)
ax.set_yticks(y,labels,color='#eee9ff')
ax.invert_yaxis()
ax.set_xlim(0,2.15)
ax.tick_params(axis='x',colors='#ddd4f2')
ax.spines[:].set_color('#5a486f')
ax.grid(axis='x',alpha=.18,zorder=0)
ax.set_xlabel('Stereo JPEG CPU decode · p50 bar, p95 whisker (ms)',color='#eee9ff')
fig.suptitle('Q20 JPEG on Pico · 544 × 544 per eye',color='#eee9ff',fontweight='bold',fontsize=18)
fig.text(.5,.015,'Standalone libjpeg-turbo 3.2.0; 12 warmups + 24 samples per job. Full 90 Hz frame = 11.11 ms.',
         ha='center',color='#ddd4f2',fontsize=10)
fig.tight_layout(rect=(0,.06,1,.92))
fig.savefig(ROOT/'pico-decode.png',dpi=160)

gpu = json.loads((ROOT/'gpu-summary.json').read_text())
series = [
    ('GPU upload · both eyes', gpu['upload_pair_gpu'], '#58d6d0'),
    ('GPU bilinear draw · both eyes', gpu['render_pair_gpu'], '#ab8ef5'),
    ('CPU submit to fence', gpu['submit_to_fence_cpu'], '#f1a879'),
]
fig, ax = plt.subplots(figsize=(10, 4.3), facecolor='#100c20')
ax.set_facecolor('#19122b')
for i, (label, values, color) in enumerate(series):
    med, tail = values['p50_ms'], values['p95_ms']
    ax.barh(i, med, height=.53, color=color, zorder=3)
    ax.errorbar(med, i, xerr=[[0], [tail-med]], fmt='none',
                ecolor='white', capsize=4, zorder=4)
    ax.text(tail+.05, i, f'{med:.3f} / {tail:.3f} ms',
            va='center', color='#eee9ff', fontsize=10)
ax.set_yticks(range(3), [row[0] for row in series], color='#eee9ff')
ax.invert_yaxis()
ax.set_xlim(0, 2.8)
ax.tick_params(axis='x', colors='#ddd4f2')
ax.spines[:].set_color('#5a486f')
ax.grid(axis='x', alpha=.18, zorder=0)
ax.set_xlabel('p50 bar, p95 whisker (ms); independent scopes', color='#eee9ff')
fig.suptitle('Pico Adreno 650 · isolated Vulkan path', color='#eee9ff',
             fontweight='bold', fontsize=18)
fig.text(.5, .015,
         'Two 544² RGBA textures → two 2176² offscreen targets. 12 warmups + 100 timed pairs; no live stream.',
         ha='center', color='#ddd4f2', fontsize=10)
fig.tight_layout(rect=(0, .06, 1, .92))
fig.savefig(ROOT/'pico-gpu.png', dpi=160)
