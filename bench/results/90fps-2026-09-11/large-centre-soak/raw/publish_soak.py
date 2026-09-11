from pathlib import Path
import json,subprocess,shutil,re,time
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path('/run/media/nerdrx/Lex/claude')
live=root/'nx-scratch/motion-live'
status=live/'large-final-soak-status.json'
while not status.exists(): time.sleep(10)
state=json.loads(status.read_text())
out=root/'nx-warp/bench/results/90fps-2026-09-11/large-centre-soak'
raw=out/'raw';raw.mkdir(parents=True,exist_ok=True)
log=live/'large-final-soak-client.log'
for name in ['large-final-soak-client.log','large-final-soak-server.log','large-final-soak-scene.log','large-final-soak-status.json','large-final-capture-status.json','ACTIVE_USER_PROFILE.json']:
 src=live/name
 if src.exists(): shutil.copy2(src,raw/name)
for script in ['analyze_warm.py','analyze_stability.py']:
 src=root/'nx-scratch/sha2-transport'/script
 shutil.copy2(src,raw/script)
 result=subprocess.check_output(['python3',str(src),str(log)],text=True)
 (raw/(script+'.json')).write_text(result)
warm=json.loads((raw/'analyze_warm.py.json').read_text())
stable=json.loads((raw/'analyze_stability.py.json').read_text())
for eye in [0,1]: shutil.copy2(root/f'nx-scratch/native-fit-live/captures/eye{eye}.png',out/f'eye{eye}.png')
rows=[]
for line in log.read_text(errors='replace').splitlines():
 m=re.search(r'render: (\d+) iterations in ([0-9.]+) s .*? (\d+) new-source',line)
 if m: rows.append(int(m[3])/float(m[2]))
fig,ax=plt.subplots(figsize=(10,4),layout='constrained')
ax.plot([2*(i+1) for i in range(len(rows))],rows,lw=1,color='#7040cc',label='Fresh selections / reported second')
ax.axhline(90,color='#bf4e30',ls='--',label='90 fresh FPS target')
ax.set(xlabel='Approximate elapsed seconds (2-second summaries)',ylabel='Fresh selections / second',title='Pico: retained 1024px native centre, 2688² output per eye',ylim=(0,100))
ax.grid(alpha=.2);ax.legend(loc='lower right');fig.savefig(out/'fresh-rate.png',dpi=160);plt.close(fig)
c=warm['client']['means'];d=warm['decoder']['means']
(out/'README.md').write_text(f"""# Large-centre motion soak — September 11

The required **1024px native sharp centre** remains selected at **2688 × 2688 output per eye**. This is the R2 two-colour, ungrouped-cell profile with smoothing 3, compact flat64, priority 1 and a 1 ms ready wait. The native unused-palette-fit encoder bypass is installed. Client revision: 87f2e9bb; decoder: 2f58ed8.

## Sustained live result

Requested duration: 900 seconds. Harness completed: **{state['complete']}**. Scene: full-field animated geometry through the custom WiVRn NX and actual Pico client. This exercises changing image content; it does not simulate every real application or prove moving-head comfort.

| Measure | Result |
|---|---:|
| Fresh selections / covered wall-second | {stable['fresh_per_covered_wall_second']:.2f} |
| Fresh selections / reported second | {stable['fresh_per_reported_second']:.2f} |
| Decode GPU summary mean | {d['nxvc_gpu_ms']:.3f} ms |
| Pass A / Pass B | {d['pass_a_ms']:.3f} / {d['pass_b_ms']:.3f} ms |
| Presentation GPU summary mean | {c['own_gpu_ms']:.3f} ms |
| Source display-time offset proxy | {c['source_offset_ms']:.2f} ms |
| Covered post-warm wall duration | {stable['covered_wall_seconds']:.2f} s |
| Largest render-summary gap | {stable['max_window_gap_seconds']:.3f} s |
| Logged session stopping events | {stable['stopping_events']} |

**90 fresh FPS is not demonstrated.** Software source-offset telemetry is not physical motion-to-photon latency. GPU values average reported windows, not per-frame percentile samples. Coverage excludes the initial 10 seconds; the graph includes startup. Two-second reporting durations are rounded.

![Fresh update history](fresh-rate.png)

## Both-eye captures

Separate 25-second capture run, excluded from timing. These are actual client-rendered eye images, not photographs through the lenses. Centre detail is visibly retained; coarse peripheral colour patches and stepped edges remain. Smoothing does not repair missing palette detail.

![Actual Pico eye 0](eye0.png)
![Actual Pico eye 1](eye1.png)

Raw logs, status, profile and analysis scripts are in [raw](raw/). The frame-time chart is based on logged fresh-source counts, not the display refresh setting.
""")
print(json.dumps({'status':state,'stable':stable,'client':c,'decoder':d},indent=2))
