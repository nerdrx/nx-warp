#!/usr/bin/env python3
"""Render reproducible model and CPU-microbenchmark results from adjacent CSV/JSON."""
from __future__ import annotations

import csv
import json
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent
NAVY = '#18324b'
BLUE = '#2878b5'
TEAL = '#238b78'
ORANGE = '#e08a32'
RED = '#b84a52'
GRAY = '#73808c'


def rows(path):
    with path.open(newline='') as f:
        return list(csv.DictReader(f))


def summary(path):
    return json.loads(path.read_text())


def add_panel_label(ax, letter, title):
    ax.set_title(f'{letter}  {title}', loc='left', fontweight='bold', color=NAVY, pad=10)


def main():
    v2 = summary(ROOT / 'v2-summary.json')
    gain104 = summary(ROOT / 'gain104-summary.json')
    fixed = [r for r in v2 if r['policy'] == 'fixed-probe-v2']
    adaptive = [r for r in v2 if r['policy'] == 'adaptive-probe-v2']
    mapped = [r for r in gain104 if r['policy'] == 'mapped-v2']
    delays = (0, 40, 100)
    capacities = (400, 550, 700)
    scenarios = [(cap, delay) for cap in capacities for delay in delays]
    key = lambda r: (r['weak_capacity_mbps'], r['feedback_delay_ms'])
    fixed_by = {key(r): r for r in fixed}
    adaptive_by = {key(r): r for r in adaptive}
    mapped_by = {key(r): r for r in mapped}
    labels = [f'{cap}\n{delay}' for cap, delay in scenarios]
    x = np.arange(len(scenarios), dtype=float)

    plt.rcParams.update({
        'font.family': 'DejaVu Sans', 'font.size': 9,
        'axes.edgecolor': '#ccd4dc', 'axes.labelcolor': NAVY,
        'xtick.color': NAVY, 'ytick.color': NAVY,
        'text.color': NAVY, 'figure.facecolor': 'white', 'axes.facecolor': '#fbfcfe',
        'axes.grid': True, 'grid.color': '#e5ebf0', 'grid.linewidth': 0.8,
        'savefig.facecolor': 'white',
    })
    fig, axs = plt.subplots(2, 2, figsize=(15, 10), constrained_layout=True)
    fig.suptitle('WiVRn NX recovery model and checker upload microbenchmark',
                 fontsize=17, fontweight='bold', color=NAVY)

    # A: all nine weak-link scenarios; show packet-loss frames during the weak window.
    ax = axs[0, 0]
    add_panel_label(ax, 'A', 'Weak-window loss across 9 model cases')
    ax.scatter(x - 0.12, [fixed_by[s]['lost_frames_40_70'] for s in scenarios],
               s=55, color=GRAY, edgecolor='white', linewidth=0.6, label='Fixed probe v2')
    ax.scatter(x + 0.12, [adaptive_by[s]['lost_frames_40_70'] for s in scenarios],
               s=58, color=BLUE, marker='D', edgecolor='white', linewidth=0.6, label='Adaptive probe v2')
    ax.set_ylabel('Lost frames, 40–70 s (frames)')
    ax.set_xticks(x, labels)
    ax.set_xlabel('Budget-equivalent capacity (Mbit/s) / feedback delay (ms)')
    ax.legend(frameon=False, ncol=2, loc='best')
    ax.text(0.02, 0.97, 'Adaptive reduces weak-window losses by ≈75%', transform=ax.transAxes,
            va='top', color=BLUE, fontweight='bold')

    # B: recovery duration in the same nine cases; include rejected 1.04 map.
    ax = axs[0, 1]
    add_panel_label(ax, 'B', 'Recovery to 1,000 Mbps across 9 model cases')
    ax.plot(x, [fixed_by[s]['recovery_seconds'] for s in scenarios], '-o', color=GRAY,
            linewidth=1.8, markersize=5, label='Fixed probe v2')
    ax.plot(x, [adaptive_by[s]['recovery_seconds'] for s in scenarios], '-D', color=BLUE,
            linewidth=2.1, markersize=5, label='Adaptive probe v2')
    ax.plot(x, [mapped_by[s]['recovery_seconds'] for s in scenarios], '--s', color=ORANGE,
            linewidth=1.8, markersize=4.5, label='1.04 mapped probe (rejected)')
    ax.set_ylabel('Recovery time (s)')
    ax.set_xticks(x, labels)
    ax.set_xlabel('Budget-equivalent capacity (Mbit/s) / feedback delay (ms)')
    ax.legend(frameon=False, fontsize=8, ncol=1, loc='upper right')
    ax.text(0.02, 0.80, 'Adaptive costs about 1 s; mapped 1.04 is slower', transform=ax.transAxes,
            va='top', color=ORANGE, fontweight='bold',
            bbox={'facecolor': 'white', 'edgecolor': 'none', 'alpha': 0.85, 'pad': 2})

    # C: one canonical adaptive trace, budget against scripted/equivalent capacity.
    ax = axs[1, 0]
    add_panel_label(ax, 'C', 'Canonical adaptive trace: budget and capacity')
    trace = rows(ROOT / 'adaptive-probe-v2.csv')
    sec = [float(r['seconds']) for r in trace]
    budget = [float(r['budget_mbps']) for r in trace]
    capacity = [float(r['equivalent_capacity_mbps']) for r in trace]
    ax.step(sec, capacity, where='post', color=ORANGE, linewidth=2, label='Link capacity')
    ax.plot(sec, budget, color=BLUE, linewidth=2.1, label='Quality budget')
    ax.set_ylabel('Budget-equivalent rate (Mbit/s)')
    ax.set_xlabel('Virtual time (s)')
    ax.set_xlim(0, max(sec))
    ax.legend(frameon=False, ncol=2, loc='lower right')
    ax.text(0.02, 0.04, 'Virtual-clock model trace; not a device capture', transform=ax.transAxes,
            color=GRAY, fontsize=8)

    # D: all twelve A/B/B/A*3 host runs and across-run medians.
    ax = axs[1, 1]
    add_panel_label(ax, 'D', 'Checker upload merge latency (host CPU)')
    bench = [r for r in rows(ROOT / 'checker-summary.csv') if r['record'] == 'run']
    bx = np.array([int(r['run']) for r in bench])
    vals = np.array([float(r['p50_ms']) for r in bench])
    base = np.array([int(r['run']) for r in bench if r['build'] == 'baseline'])
    cand = np.array([int(r['run']) for r in bench if r['build'] == 'candidate'])
    bvals = np.array([float(r['p50_ms']) for r in bench if r['build'] == 'baseline'])
    cvals = np.array([float(r['p50_ms']) for r in bench if r['build'] == 'candidate'])
    base_med = float(np.median(bvals))
    cand_med = float(np.median(cvals))
    ax.scatter(base, bvals, s=55, color=GRAY, edgecolor='white', label='Baseline')
    ax.scatter(cand, cvals, s=58, color=TEAL, marker='D', edgecolor='white', label='Endian-safe store')
    ax.axhline(base_med, color=GRAY, linestyle='--', linewidth=1.5,
               label=f'Baseline median {base_med:.3f} ms')
    ax.axhline(cand_med, color=TEAL, linestyle='--', linewidth=1.5,
               label=f'Candidate median {cand_med:.3f} ms')
    ax.set_ylabel('Merge p50 (ms)')
    ax.set_xlabel('Run number (A/B/B/A repeated 3×)')
    ax.set_xticks(range(1, 13))
    ax.set_xlim(0.5, 12.5)
    ax.legend(frameon=False, fontsize=8, loc='upper right')
    pct = (cand_med / base_med - 1) * 100
    ax.text(0.02, 0.50, f'{pct:.1f}% median reduction • identical checksum • 535,376 B',
            transform=ax.transAxes, color=TEAL, fontsize=8, fontweight='bold',
            bbox={'facecolor': 'white', 'edgecolor': 'none', 'alpha': 0.85, 'pad': 2})

    fig.savefig(ROOT / 'recovery-headroom.png', dpi=220, bbox_inches='tight')
    fig.savefig(ROOT / 'recovery-headroom.svg', bbox_inches='tight')
    svg = ROOT / 'recovery-headroom.svg'
    svg.write_text('\n'.join(line.rstrip() for line in svg.read_text().splitlines()) + '\n')
    plt.close(fig)


if __name__ == '__main__':
    main()
