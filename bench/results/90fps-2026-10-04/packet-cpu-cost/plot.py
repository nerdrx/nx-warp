from pathlib import Path
import json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

p = Path(__file__).parent
rows = json.loads((p / 'results.json').read_text())['rows']
fig, axes = plt.subplots(2, 2, figsize=(10, 6), constrained_layout=True)
for row, scene in enumerate(('dark', 'forest')):
    for k, (kind, color) in enumerate(zip(('lz4', 'zstd1', 'zstd3'), ('#d97732', '#9657d7', '#305896'))):
        data = [next(x for x in rows if x['scene'] == scene and x['q'] == q and x['compression'] == kind) for q in (2, 4, 6)]
        pos = np.arange(3) + (k - 1) * .25
        axes[row, 0].bar(pos, [x['median_ms'] for x in data], .23, color=color, label=kind)
        axes[row, 0].scatter(pos, [x['p95_ms'] for x in data], s=12, color='#111827', marker='_')
        axes[row, 1].bar(pos, [x['packed_bytes'] / 1000 for x in data], .23, color=color)
    for column, label in enumerate(('PC compression time (ms)', 'Compressed ASTC payload (kB)')):
        ax = axes[row, column]; ax.set_xticks(range(3), ('q2', 'q4', 'q6'))
        ax.set_ylabel(label); ax.set_title(scene.capitalize()); ax.grid(axis='y', alpha=.2)
        ax.set_axisbelow(True)
axes[0, 0].legend(frameon=False, ncol=3)
fig.suptitle('Lossless packet packing: time versus bytes\nTwo 1920×1080 photo-derived ASTC payloads; 12 warmups, 30 samples; marks show p95', fontsize=12)
for suffix in ('png', 'svg'):
    fig.savefig(p / f'compression-cost.{suffix}', dpi=170)
svg = p / 'compression-cost.svg'
svg.write_text('\n'.join(line.rstrip() for line in svg.read_text().splitlines()) + '\n')
