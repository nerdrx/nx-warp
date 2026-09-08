#!/usr/bin/env python3
"""Plot raw scheduled-arrival latency and oldest retained tile age."""
import csv, io, tarfile
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root = Path(__file__).parent
fig, axes = plt.subplots(2, 1, figsize=(11, 6), sharex=True, constrained_layout=True)
with tarfile.open(root / 'raw-inputs.tar.gz') as archive:
    for name, label in [('full-v2', 'Full draw'), ('foveated-v2-a', 'Centre-first A'), ('foveated-v2-b', 'Centre-first B')]:
        rows = list(csv.DictReader(io.TextIOWrapper(archive.extractfile(name + '.csv'))))
        t = [int(r['frame']) / 90 for r in rows]
        axes[0].plot(t, [float(r['total_ms']) for r in rows], label=label, linewidth=.7, alpha=.8)
        axes[1].plot(t, [float(r['peripheral_age_max']) / 90 * 1000 for r in rows], linewidth=.8)
axes[0].axhline(1000/90, color='red', linestyle='--', label='90 Hz deadline')
axes[0].set_ylabel('Arrival to GPU completion (ms)')
axes[0].legend(ncol=4, fontsize=8)
axes[1].set_ylabel('Oldest retained tile (ms)')
axes[1].set_xlabel('Scheduled source time (s)')
fig.suptitle('Pico 4 · 4352 × 2176 stereo · synthetic camera motion · offscreen')
fig.savefig(root / 'latency-and-age.png', dpi=160)
