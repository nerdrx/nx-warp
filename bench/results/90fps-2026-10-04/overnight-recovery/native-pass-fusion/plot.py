from pathlib import Path
import csv
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

base = Path(__file__).resolve().parent
rows = list(csv.DictReader((base / 'scope-alternating.csv').open()))
variants = ['baseline', 'fused']
fig, axes = plt.subplots(1, 2, figsize=(10, 5), layout='constrained')
bottom = np.zeros(2)
for field, label, colour in [('pass_gpu_us', 'Identity colour pass', '#19a1a6'), ('astc_gpu_us', 'ASTC interval', '#6657d4'), ('copy_gpu_us', 'Readback copy interval', '#9ea6b4')]:
    values = np.array([np.mean([float(r[field]) for r in rows if r['variant'] == v]) / 1000 for v in variants])
    axes[0].bar(variants, values, bottom=bottom, label=label, color=colour, width=.58)
    bottom += values
for x, mean in enumerate(bottom):
    axes[0].text(x, mean+.03, f'{mean:.3f} ms', ha='center')
axes[0].set_ylim(0, 2.05)
axes[0].set_ylabel('Mean GPU interval (ms)')
axes[0].legend(loc='upper left', fontsize=9)
axes[0].set_title('Removing a pass adds repeated colour math')
for variant, x, colour in [('baseline', 0, '#19a1a6'), ('fused', 1, '#6657d4')]:
    values = [float(r['cpu_wall_us']) / 1000 for r in rows if r['variant'] == variant]
    axes[1].scatter(np.linspace(x-.08, x+.08, len(values)), values, color=colour, s=23)
for pair in range(12):
    rs = {r['variant']:r for r in rows if int(r['pair']) == pair}
    axes[1].plot([0, 1], [float(rs[v]['cpu_wall_us'])/1000 for v in variants], color='#888888', alpha=.35, zorder=0)
axes[1].set_xticks([0, 1], variants)
axes[1].set_ylabel('Host record-tail / submit / fence time (ms)')
axes[1].set_ylim(0, 2.05)
axes[1].set_title('Each of 12 measured pairs is slower')
for ax in axes:
    ax.spines[['top', 'right']].set_visible(False)
    ax.grid(axis='y', alpha=.2)
fig.suptitle('Naive native pass fusion: rejected in this scope\n2176², q6/fit3, one eye · byte-identical ASTC output', fontsize=14)
fig.supxlabel('Synthetic RGBA32F identity input; no rect/flip/mask, application, network or headset timing.', fontsize=9)
fig.savefig(base/'fusion.png', dpi=160)
