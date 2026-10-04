from pathlib import Path
import csv
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

base = Path(__file__).resolve().parent
fig, axes = plt.subplots(2, 2, figsize=(11, 7), layout='constrained')
for i, (file, title) in enumerate([('scope-alternating.csv', 'Normal run'), ('validation-alternating.csv', 'Separate validation run')]):
    rows = [r for r in csv.DictReader((base / file).open()) if r['warmup'] == '0']
    for j, (field, factor, label) in enumerate([('cpu_fence_wait_ms', 1, 'CPU fence wait (ms)'), ('gpu_compute_through_readback_ms', 1000, 'GPU compute through readback (µs)')]):
        ax = axes[i, j]
        for delay, colour in [(0, '#6553d9'), (10, '#138f96')]:
            values = np.array([float(r[field]) * factor for r in rows if int(r['delay_ms']) == delay])
            assert len(values) == 12
            x = (0 if delay == 0 else 1)
            ax.scatter(x + np.linspace(-.09, .09, len(values)), values, color=colour, alpha=.75, s=23)
            ax.plot([x-.18, x+.18], [values.mean()]*2, color=colour, linewidth=3)
            ax.text(x, values.max() + (.45 if j == 0 else .4), f'mean {values.mean():.3f}', ha='center', fontsize=10)
        ax.set_title(title + ' · ' + ('host clock' if j == 0 else 'GPU clock'))
        ax.set_xticks([0, 1], ['0 ms host delay', '10 ms host delay'])
        ax.set_ylabel(label)
        ax.set_xlim(-.5, 1.5)
        ax.set_ylim(bottom=0, top=14 if j == 0 else 11)
        ax.grid(axis='y', alpha=.2)
        ax.spines[['top', 'right']].set_visible(False)
fig.suptitle('The input wait precedes the GPU timestamp bracket\n12 measured submissions per condition · points = samples · bars = means', fontsize=15)
fig.supxlabel('Generated 64 KiB compute/copy check; all generation checks pass. Scope validation only, not codec timing or latency.', fontsize=10)
fig.savefig(base / 'scope.png', dpi=160)
