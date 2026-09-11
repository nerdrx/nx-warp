from pathlib import Path
import subprocess,json,re,shutil
r=Path('/run/media/nerdrx/Lex/claude');live=r/'nx-scratch/motion-live';out=r/'nx-warp/bench/results/90fps-2026-09-11/hevc-motion';raw=out/'raw';raw.mkdir(parents=True,exist_ok=True)
rows=[]
for label in ['hevc-motion-off-a','hevc-motion-on-a','hevc-motion-off-b','hevc-motion-final']:
 src=live/(label+'-client.log');row=json.loads(subprocess.check_output(['python3',str(r/'nx-scratch/sha2-transport/analyze_warm.py'),str(src)],text=True))
 matched=active=0;weighted=0.;start=None
 for line in src.read_text(errors='replace').splitlines():
  tm=re.search(r'\d\d-\d\d (\d\d):(\d\d):(\d\d\.\d+)',line)
  if not tm:continue
  t=float(tm[1])*3600+float(tm[2])*60+float(tm[3])
  if 'render:' in line and 'iterations in' in line and start is None:start=t
  m=re.search(r'motion fields matched (\d+) active (\d+) mean active step ([\d.]+)',line)
  if m and start is not None and (t-start)%86400>=10:
   matched+=int(m[1]);active+=int(m[2]);weighted+=int(m[2])*float(m[3])
 row['motion']={'matched':matched,'active':active,'mean_active_step':weighted/active if active else 0};rows.append(row)
 for suffix in ['client.log','server.log','status.json']:
  p=live/(label+'-'+suffix);shutil.copy2(p,raw/p.name)
(raw/'summary.json').write_text(json.dumps(rows,indent=2))
for name in ['run.py','final_probe.py','trials.log','server-build.log','apk-final-build.log']:shutil.copy2(r/'nx-scratch/hevc-motion'/name,raw/name)
shutil.copy2(r/'nx-scratch/sha2-transport/analyze_warm.py',raw/'analyze_warm.py')
lines=['# Full HEVC with headset-side motion warp','', 'An opt-in prototype uses the existing Qualcomm HEVC decoder for the full image and applies the existing WiVRn motion-field warp inside its presentation pass. This is a hybrid streaming experiment, not a new independent compression format. The fields come from the PC motion estimator; the MediaCodec API does not expose arbitrary motion-vector arithmetic.','', '## Implementation','', '- Explicit test override for headset motion mode, without changing saved user settings.','- Opt-in server field generation even when the application itself keeps up; existing failure and unsafe-submission guards remain.','- Retain eight completed motion fields so a decoded image can find its own matching field rather than only the newest one.','- Require a matching frame across both eyes and clear field history on decoder reset.','- Count matching fields, active warps and extrapolation steps. A step of 1 means one source-frame interval.','', 'The existing runtime head-pose reprojection still applies. This screen uses a stationary headset and animated full-field content; it does not prove improved head-motion responsiveness, depth-aware translation or disocclusion handling.','', '## Short screens','', 'All timing runs last 30 seconds at 2688² per eye with 10-bit HEVC. Values below discard the initial 10 seconds of telemetry. The final run includes the stereo/reset guards.','', '| Run | Fresh selections/s | Presentation GPU ms | Matched fields | Active warps | Mean active step |','|---|---:|---:|---:|---:|---:|']
for row in rows:
 c=row['client']['means'];m=row['motion'];lines.append(f"| {Path(row['file']).stem} | {c['fresh_per_s']:.2f} | {c['own_gpu_ms']:.2f} | {m['matched']} | {m['active']} | {m['mean_active_step']:.4f} |")
lines+=['','Matching and nonzero warp activity demonstrate that the prototype executes. **They do not demonstrate a latency or perceptual improvement.** Many predicted display times are at or before the source frame’s stamped display time, so the safe extrapolation amount is zero or very small. Do not force a larger step simply to make the effect visible; the timestamp meaning and visual error must justify it.','','Fresh counts are the existing first-eye source selection metric, not proof of 90 fresh stereo frames or reduced physical motion-to-photon latency. The short off/on/off sequence includes drift and is not a statistically isolated performance win. Fields cannot supply newly revealed scene content.','','## Disposition and reproduction','', 'Keep this as an opt-in experimental path. The original NX large-centre profile is restored after tests. Set server environment WIVRN_NX_ALWAYS_MOTION_FIELD=1 and client debug.wivrn.nx.motion_mode=headset only for the prototype; default or an absent client property follows saved settings. Server forcing applies only to headset mode. Do not stack an extra rotational warp without accounting for the runtime’s existing reprojection.','','The initial trial script failed while clearing an empty ADB property after all three timing runs completed; the override and original server configuration were restored manually. The script now restores the explicit default sentinel. This orchestration failure is retained in the raw log.','','Raw logs, scripts and build records are in [raw](raw/). Separate eye captures, if shown below, are excluded from timing.']
(out/'README.md').write_text(chr(10).join(lines)+chr(10))
