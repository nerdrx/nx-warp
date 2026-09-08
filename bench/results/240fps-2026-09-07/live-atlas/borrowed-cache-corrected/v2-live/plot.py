#!/usr/bin/env python3
"""Plot the four repeated HEVC/NX comparisons from the archived measurements."""
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
records = json.loads((root / 'pipeline-summary.json').read_text())[:4]
fig, ax = plt.subplots(figsize=(9, 4.8), layout='constrained')
colors = ['#41556e', '#7540ba', '#41556e', '#7540ba']
labels = ['HEVC · first pair', 'NX cache · first pair', 'HEVC · reverse pair', 'NX cache · reverse pair']
for y, (record, color) in enumerate(zip(records, colors)):
    p50, p95, p99 = record['stages']['blit']['p50_p95_p99_ms']
    ax.plot([p50, p99], [y, y], color=color, linewidth=3, alpha=.55)
    for value, marker in zip([p50, p95, p99], ['o', '|', 'D']):
        ax.plot(value, y, marker, color=color, markersize=8)
    ax.text(p99 + .6, y, f'{p50:.2f} / {p95:.2f} / {p99:.2f}', va='center', fontsize=9)
ax.set_yticks(range(4), labels)
ax.invert_yaxis()
ax.set_xlim(0, 62)
ax.set_xlabel('Encode start → render selection (ms) · lower is better')
ax.set_title('Native-resolution Pico 4: repeated pipeline comparison', loc='left', weight='bold')
ax.grid(axis='x', alpha=.18)
ax.spines[['top', 'right', 'left']].set_visible(False)
fig.text(.01, -.04, '● p50   | p95   ◆ p99 — percentiles, not confidence intervals.\nDifferent bitrate and image quality; not photon latency or 240 FPS.', fontsize=9)
fig.savefig(root / 'pipeline-comparison.png', dpi=180, bbox_inches='tight', facecolor='white')
