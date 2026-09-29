"""Published numeric figures; source photos and visual comparisons stay private."""
import csv
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).parent
rows = list(csv.DictReader((ROOT/'quality.csv').open()))
plt.rcParams.update({'figure.facecolor': '#100c20', 'axes.facecolor': '#19122b',
                     'text.color': '#eee9ff', 'axes.labelcolor': '#eee9ff',
                     'xtick.color': '#ddd4f2', 'ytick.color': '#ddd4f2',
                     'axes.edgecolor': '#5a486f', 'font.size': 10,
                     'savefig.facecolor': '#100c20'})


def mean(scene, side, q, chroma, key):
    values = [float(r[key]) for r in rows if r['fixture'].startswith(scene)
              and int(r['jpeg_side']) == side and int(r['quality']) == q
              and int(r['chroma']) == chroma]
    assert len(values) == 3
    return np.mean(values)


fig, axes = plt.subplots(1, 2, figsize=(13, 5.6))
colors = ['#b48cff', '#53dbc9', '#ffc16b']
for scene, ax in zip(('forest', 'dark'), axes):
    samples = [r for r in rows if r['fixture'].startswith(scene)]
    nx_rate = np.mean([float(r['nx_mbps90']) for r in samples])
    nx_quality = np.mean([float(r['nx_outer_psnr_db']) for r in samples])
    ax.scatter([nx_rate], [nx_quality], marker='*', s=190, c='#f8799c', label='Current NXVC')
    for side, color in zip((272, 544, 1088), colors):
        for chroma, linestyle in ((420, '-'), (444, ':')):
            rates = [mean(scene, side, q, chroma, 'mbps90') for q in (5, 10, 20, 30)]
            quality = [mean(scene, side, q, chroma, 'outer_psnr_db') for q in (5, 10, 20, 30)]
            ax.plot(rates, quality, linestyle, marker='o', color=color,
                    label=f'{side}²/eye · {chroma}')
            if chroma == 420:
                for x, y, q in zip(rates, quality, (5, 10, 20, 30)):
                    ax.annotate(f'Q{q}', (x, y), xytext=(4, -12), textcoords='offset points', fontsize=8)
    ax.set(title=f'{scene.title()} scene · mean of three source shifts',
           xlabel='Stereo payload at 90 frames/s (Mbit/s)',
           ylabel='Outside blend region: RGB PSNR to source (dB)')
    ax.grid(alpha=.15)
    ax.legend(fontsize=8, loc='lower right')
fig.suptitle('NXVC centre + low-quality JPEG outside', fontsize=19, fontweight='bold', y=.99)
fig.text(.5, .012, 'Includes centre, JPEG, 36-byte header and common safety image. File-size normalization; no live throughput claim.',
         ha='center', fontsize=9)
fig.tight_layout(rect=(0, .045, 1, .95))
fig.savefig(ROOT/'rate-quality.png', dpi=160)
plt.close(fig)

y, x = np.mgrid[:2176, :2176]
r = np.hypot(x-1087.5, y-1087.5)
t = np.clip((r-128)/256, 0, 1)
w = 1-t*t*t*(t*(6*t-15)+10)
fig, axes = plt.subplots(1, 2, figsize=(11, 4.7), gridspec_kw={'width_ratios': [1, 1.5]})
axes[0].imshow(w, cmap='magma', vmin=0, vmax=1)
axes[0].set(title='NXVC contribution · one eye', xlabel='2176 output pixels', xticks=[], yticks=[])
for radius in (128, 384):
    axes[0].add_patch(plt.Circle((1087.5, 1087.5), radius, fill=False, color='white', alpha=.5, linewidth=.7))
axes[1].plot(np.arange(2176)-1087.5, w[1088], color='#b48cff', lw=2, label='NXVC centre')
axes[1].plot(np.arange(2176)-1087.5, 1-w[1088], color='#53dbc9', lw=2, label='JPEG periphery')
axes[1].set(xlim=(-650, 650), ylim=(-.02,1.02), xlabel='Distance from centre (output pixels)', ylabel='Blend weight')
axes[1].grid(alpha=.15); axes[1].legend()
fig.suptitle('Exact existing centre; gradual round transition', fontsize=17)
fig.text(.5, .015, 'r ≤ 128: exact decoded NXVC pixels. r = 128–384: quintic blend. r ≥ 384: JPEG only.', ha='center', fontsize=10)
fig.tight_layout(rect=(0, .06, 1, .94))
fig.savefig(ROOT/'blend.png', dpi=160)
plt.close(fig)

timings = list(csv.DictReader((ROOT/'jpeg-decode-summary.csv').open()))
fig, axes = plt.subplots(1, 2, figsize=(11, 4.8), sharey=True)
for ax, scene in zip(axes, ('forest', 'dark')):
    for offset, q, color in ((-.18, 10, '#53dbc9'), (.18, 20, '#b48cff')):
        values = [next(row for row in timings if row['fixture'] == scene+'-s0'
                       and int(row['jpeg_side']) == side and int(row['quality']) == q
                       and row['chroma'] == '420') for side in (272, 544, 1088)]
        medians = np.array([float(v['decode_p50_us'])/1000 for v in values])
        tails = np.array([float(v['decode_p95_us'])/1000 for v in values])
        positions = np.arange(3)+offset
        ax.bar(positions, medians, width=.34, color=color, label=f'Q{q} · p50')
        ax.errorbar(positions, medians, yerr=[np.zeros(3), tails-medians],
                    fmt='none', ecolor='white', capsize=4)
    ax.set(title=scene.title(), xticks=np.arange(3), xticklabels=('272²','544²','1088²'),
           xlabel='JPEG dimensions per eye')
    ax.grid(axis='y', alpha=.15); ax.legend()
axes[0].set_ylabel('Stereo JPEG → RGB decode (ms)')
fig.suptitle('Host JPEG decode cost · bars p50, whiskers p95', fontsize=17)
fig.text(.5, .012, 'Ryzen 9 9950X3D · 4:2:0 · background applications present. Excludes NX decode, GPU work and display.',
         ha='center', fontsize=9)
fig.tight_layout(rect=(0, .05, 1, .94))
fig.savefig(ROOT/'decode-cost.png', dpi=160)
plt.close(fig)
