import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=Path(__file__).parent; d=json.loads((p/'timing.json').read_text());r=d['rows']
fig,ax=plt.subplots(figsize=(9,4));ax.plot([x['mean_ms'] for x in r],label='Window mean');ax.plot([x['max_ms'] for x in r],label='Window maximum',alpha=.7);ax.set(xlabel='Two-second telemetry window (first five excluded)',ylabel='CPU wall time (ms)',title='2688² per eye: selection → presentation-pass entry');ax.legend();ax.grid(alpha=.2);fig.tight_layout();fig.savefig(p/'selection-delay.png',dpi=150)
