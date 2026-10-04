#!/usr/bin/env python3
"""Plot public-safe band packet-loss and byte estimates from aggregate CSVs."""
import csv
from pathlib import Path
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
with (HERE / 'native_band_payload.csv').open() as f:
    payload = list(csv.DictReader(f))
with (HERE / 'native_band_loss.csv').open() as f:
    losses = list(csv.DictReader(f))
labels = ['whole', 'band_256px', 'band_512px', 'band_1024px']
ticks = ['Whole frame', '256 px bands', '512 px bands', '1024 px bands']
colors = {'whole': '#477caf', 'band_256px': '#c4b641', 'band_512px': '#328347', 'band_1024px': '#65c3df'}
fig, axes = plt.subplots(2, 2, figsize=(12, 8), constrained_layout=True)
for row, sample in enumerate(('dark_fixture', 'forest_fixture')):
    p = {r['mode']: r for r in payload if r['sample'] == sample}
    ax = axes[row, 0]
    vals = [int(p[m]['total_estimated_payload_bytes']) / 1024 for m in labels]
    bars = ax.bar(range(4), vals, color=[colors[m] for m in labels])
    ax.set_xticks(range(4), ticks)
    ax.set_ylabel('Estimated payload + parity (KiB)')
    ax.set_title(('Dark' if row == 0 else 'Forest') + ' fixture: estimated bytes')
    for bar, mode in zip(bars, labels):
        pct = float(p[mode]['penalty_vs_whole_percent'])
        ax.text(bar.get_x()+bar.get_width()/2, bar.get_height()+3,
                'baseline' if mode == 'whole' else f'+{pct:.1f}%', ha='center', fontsize=9)
    ax = axes[row, 1]
    for loss_model, color, offset in [('iid_2pct', '#477caf', -0.14), ('iid_5pct', '#c4b641', 0.14)]:
        values=[]
        for mode in labels:
            item=next(r for r in losses if r['sample']==sample and r['mode']==mode and r['loss_model']==loss_model)
            values.append(float(item['mean_recovered_area_per_send'])*100)
        bars=ax.bar([i+offset for i in range(4)], values, width=.27, color=color, label=loss_model.replace('_',' ').upper())
        for bar, value in zip(bars, values):
            ax.text(bar.get_x()+bar.get_width()/2, value+1.0, f'{value:.0f}%', ha='center', fontsize=8)
    ax.set_xticks(range(4), ticks)
    ax.set_ylim(0, 112)
    ax.set_ylabel('Modeled recovered area per send (%)')
    ax.set_title(('Dark' if row == 0 else 'Forest') + ' fixture: IID packet loss (blue 2%, gold 5%)')
fig.suptitle('Full-width ASTC bands trade bytes for partial recovery\n2176×2176 static snapshots · 300×36 repeated sends · model, not motion or live throughput', fontsize=13)
for ax in axes.flat:
    ax.grid(axis='y', alpha=.22)
    ax.set_axisbelow(True)
for ext in ('png','svg'):
    fig.savefig(HERE / f'native_band_tradeoff.{ext}', dpi=160)
print(HERE / 'native_band_tradeoff.png')
