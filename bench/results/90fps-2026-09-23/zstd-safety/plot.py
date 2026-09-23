#!/usr/bin/env python3
import csv, json, re, statistics, sys
from datetime import datetime, timedelta
from pathlib import Path
import matplotlib.pyplot as plt

if len(sys.argv) > 2:
    raise SystemExit('usage: plot.py [PRIVATE_LOG]')
out = Path(__file__).parent
pattern = re.compile(r'NX safety: (fallback|detail) at source \d+ frame (\d+) after ([0-9.]+) ms primary hold')
events = []
if len(sys.argv) == 2:
    stamp = re.compile(r'^09-23 (\d\d:\d\d:\d\d\.\d+)')
    upload = re.compile(r'NXWARP_BENCH_RGBA8 uploaded')
    log_path = Path(sys.argv[1])
    scene_path = log_path.with_name(log_path.name.replace('logcat-', 'scene-', 1))
    marker_time = None
    for line in scene_path.read_text(errors='replace').splitlines():
        m = re.search(r'\[(\d\d:\d\d:\d\d\.\d+)\]', line)
        if m and upload.search(line):
            marker_time = datetime.strptime(m.group(1), '%H:%M:%S.%f')
            break
    if marker_time is None:
        raise SystemExit('RGBA8 upload marker not found in paired scene log')
    steady_after = marker_time + timedelta(seconds=10)
    for line in log_path.read_text(errors='replace').splitlines():
        tm = stamp.match(line)
        m = pattern.search(line)
        if m and tm and datetime.strptime(tm.group(1), '%H:%M:%S.%f') >= steady_after:
            frame = int(m.group(2))
            events.append({'kind': m.group(1), 'frame': frame, 'frame_mod_180': frame % 180,
                           'hold_ms': float(m.group(3))})
else:
    with (out / 'events.csv').open(newline='') as f:
        for row in csv.DictReader(f):
            events.append({'kind': row['kind'], 'frame': int(row['frame']),
                           'frame_mod_180': int(row['frame_mod_180']),
                           'hold_ms': float(row['hold_ms'])})
if not events:
    raise SystemExit('no NX safety events found')
with (out / 'events.csv').open('w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=events[0].keys()); w.writeheader(); w.writerows(events)
summary = {'source': 'photo-safety-crowd-loss1', 'detail_loss': '30 of every 180 source frames',
           'filter': 'events at least 10 seconds after the RGBA8 upload marker',
           'events': len(events), 'kinds': {}}
for kind in ('fallback', 'detail'):
    vals = [e['hold_ms'] for e in events if e['kind'] == kind]
    mods = [e['frame_mod_180'] for e in events if e['kind'] == kind]
    summary['kinds'][kind] = {'count': len(vals), 'median_ms': statistics.median(vals),
                              'p95_ms': sorted(vals)[max(0, int(.95 * len(vals)) - 1)],
                              'max_ms': max(vals), 'frame_mod_180': mods}
summary['detail_return_mod_180_counts'] = {str(k): v for k, v in sorted(__import__('collections').Counter(
    e['frame_mod_180'] for e in events if e['kind'] == 'detail').items())}
summary['detail_return_phase_groups'] = {
    'mod_90_or_91': sum(e['frame_mod_180'] in (90, 91) for e in events if e['kind'] == 'detail'),
    'other_mod_180': sum(e['frame_mod_180'] not in (90, 91) for e in events if e['kind'] == 'detail'),
}
(out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
windows = []
for start in range((min(e['frame'] for e in events) // 180) * 180, max(e['frame'] for e in events) + 180, 180):
    row = {'frame_start': start, 'frame_end': start + 179}
    for kind in ('fallback', 'detail'):
        vals = [e['hold_ms'] for e in events if e['kind'] == kind and start <= e['frame'] < start + 180]
        row[kind + '_count'] = len(vals)
        row[kind + '_median_ms'] = statistics.median(vals) if vals else None
        row[kind + '_max_ms'] = max(vals) if vals else None
    windows.append(row)
(out / 'windows.json').write_text(json.dumps(windows, indent=2) + '\n')
fig, ax = plt.subplots(1, 2, figsize=(12, 4.8), dpi=160)
for kind, color in [('fallback', '#777777'), ('detail', '#7700ff')]:
    a = [e for e in events if e['kind'] == kind]
    ax[0].scatter([e['frame'] for e in a], [e['hold_ms'] for e in a], label=kind, s=18, color=color)
    ax[1].scatter([e['frame_mod_180'] for e in a], [e['hold_ms'] for e in a], label=kind, s=18, color=color)
ax[0].set(xlabel='source frame index', ylabel='primary hold (ms)', title='Safety hold by frame')
ax[1].set(xlabel='frame index mod 180', ylabel='primary hold (ms)', title='Recovery phase')
for a in ax:
    a.grid(axis='both', alpha=.25); a.legend()
fig.suptitle('NX safety synthetic detail loss — sanitized timing')
fig.tight_layout(rect=(0, 0, 1, .94)); fig.savefig(out / 'comparison.png')
