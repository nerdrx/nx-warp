#!/usr/bin/env python3
"""Render public-safe byte-cost and modeled fresh-area figures from numeric CSVs."""
import argparse
import csv
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np


def read_csv(path):
    with path.open(newline='') as f:
        return list(csv.DictReader(f))


def payload_figure(root):
    rows = read_csv(root / 'native_payload_bytes.csv')
    samples = [('dark_q6', 'Dark fixture'), ('forest_q6', 'Forest fixture')]
    modes = ['whole', 'region_64', 'region_128', 'region_256']
    colors = ['#4477AA', '#66CCEE', '#228833', '#CCBB44']
    fig, axes = plt.subplots(1, 2, figsize=(10.2, 4.8), sharey=True, layout='constrained')
    for ax, (sample, label) in zip(axes, samples):
        values = {r['mode']: r for r in rows if r['sample'] == sample}
        bars = ax.bar(range(4), [int(values[m]['total_payload_bytes_est']) / 1024 for m in modes],
                      color=colors, width=.68)
        for bar, mode in zip(bars, modes):
            penalty = float(values[mode]['penalty_vs_whole_percent'])
            suffix = 'baseline' if mode == 'whole' else f'+{penalty:.1f}%'
            ax.annotate(suffix, (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                        xytext=(0, 4), textcoords='offset points', ha='center', fontsize=8)
        ax.set_title(label)
        ax.set_xticks(range(4), ['Whole', '64 px', '128 px', '256 px'])
        ax.set_ylabel('Estimated payload + parity (KiB)')
        ax.grid(axis='y', alpha=.22)
        ax.set_axisbelow(True)
    fig.suptitle('Region size trades bandwidth for partial recovery\n'
                 'Static 2176² ASTC payloads · Zstd 3 · 1400 B fragments · 8+1 XOR FEC')
    for ext in ('png', 'svg'):
        fig.savefig(root / f'native_payload_penalty.{ext}', dpi=180, bbox_inches='tight')
    plt.close(fig)


def freshness_figure(root):
    rows = read_csv(root / 'native_loss_scaling.csv')
    samples = [('dark_q6', 'Dark fixture'), ('forest_q6', 'Forest fixture')]
    scenarios = [('iid_2pct', 'IID 2%'), ('iid_5pct', 'IID 5%')]
    methods = [('whole', 'Whole frame', '#4477AA'), ('region_256', '256 px regions', '#CCBB44')]
    fig, axes = plt.subplots(1, 2, figsize=(9.8, 5.2), sharey=True)
    width = .34
    for ax, (sample, sample_label) in zip(axes, samples):
        subset = {(r['loss_model'], r['mode']): r for r in rows if r['sample'] == sample}
        x = np.arange(len(scenarios))
        for method_index, (mode, label, color) in enumerate(methods):
            vals = [100 * float(subset[(scenario, mode)]['mean_fresh_area_fraction'])
                    for scenario, _ in scenarios]
            bars = ax.bar(x + (method_index - .5) * width, vals, width, label=label, color=color)
            for bar, value in zip(bars, vals):
                ax.annotate(f'{value:.1f}%', (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                            xytext=(0, 4), textcoords='offset points', ha='center', fontsize=8)
        ax.set_title(sample_label)
        ax.set_xticks(x, [label for _, label in scenarios])
        ax.set_ylim(0, 112)
        ax.set_ylabel('Modeled recovered fresh area per send (%)')
        ax.grid(axis='y', alpha=.22)
        ax.set_axisbelow(True)
    fig.legend(*axes[0].get_legend_handles_labels(), loc='lower center', ncol=2, frameon=False,
               bbox_to_anchor=(.5, .015))
    fig.subplots_adjust(top=.80, bottom=.20, left=.09, right=.99, wspace=.08)
    fig.suptitle('Partial region delivery preserves area under packet loss\n'
                 'Modeled · unchanged native ASTC frame repeated 36× · not motion quality or live throughput')
    for ext in ('png', 'svg'):
        fig.savefig(root / f'native_fresh_area.{ext}', dpi=180, bbox_inches='tight')
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-dir', type=Path, default=Path(__file__).resolve().parent)
    root = parser.parse_args().data_dir
    payload_figure(root)
    freshness_figure(root)
    print(root / 'native_payload_penalty.png')
    print(root / 'native_fresh_area.png')


if __name__ == '__main__':
    main()
