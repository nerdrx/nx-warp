#!/usr/bin/env python3
"""Extract compact live ASTC evidence from the four presentation and server logs."""
import csv,hashlib,json,re,statistics
from pathlib import Path
ROOT=Path(__file__).resolve().parent
SRC=Path('/run/media/nerdrx/Lex/claude/nx-scratch/astc-half-rate-20261004/live-check')
mode_logs=('presentation-mode0.log','presentation-mode0-repeat.log','presentation-mode2.log','presentation-mode2-repeat.log')
iter_re=re.compile(r'render: (\d+) iterations in ([\d.]+) s \(([\d.]+)/s\), \d+ submitted a layer, (\d+) new-source')
gpu_re=re.compile(r"this app's own GPU pass ([\d.]+) ms per iteration")
miss_re=re.compile(r'misses:\s*(\d+)\s+overrun\s+(\d+)\s+late\s+(\d+)\s+skipped refresh')
def key(line):
 a=line.split();return ' '.join(a[:2]) if len(a)>2 else ''
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
rows=[]
for name in mode_logs:
 p=SRC/name; lines=p.read_text(errors='replace').splitlines(); by={}; latest=None
 for i,line in enumerate(lines,1):
  k=key(line)
  if not k:continue
  if (m:=iter_re.search(line)):
   latest=(k,i)
   by.setdefault(k,{}).update({'iterations':int(m[1]),'seconds':float(m[2]),'iterations_per_second':float(m[3]),'new_source':int(m[4]),'iteration_line':i})
  if (m:=gpu_re.search(line)) and latest and i-latest[1]<=8:
   by.setdefault(latest[0],{}).update({'app_gpu_pass_ms':float(m[1]),'gpu_line':i})
  if (m:=miss_re.search(line)):
   by.setdefault(k,{}).update({'overruns':int(m[1]),'late':int(m[2]),'refresh_skips':int(m[3])})
 mode=int(re.search(r'mode(\d+)',name)[1]); run='repeat' if 'repeat' in name else 'first'
 for timestamp,x in by.items():
  if x.get('iterations',0)>=170 and 'app_gpu_pass_ms' in x:
   rows.append({'mode':mode,'run':run,'timestamp':timestamp,'iterations':x['iterations'],'seconds':x['seconds'],'iterations_per_second':x['iterations_per_second'],'new_source_frames':x['new_source'],'fresh_per_second':round(x['new_source']/x['seconds'],2),'app_gpu_pass_ms':x['app_gpu_pass_ms'],'overruns':x.get('overruns'),'late':x.get('late'),'refresh_skips':x.get('refresh_skips'),'source_log':name,'source_log_sha256':sha(p),'iteration_line':x['iteration_line'],'gpu_line':x['gpu_line']})
fields=list(rows[0])
with (ROOT/'presentation-gpu-pass.csv').open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=fields,lineterminator='\n');w.writeheader();w.writerows(rows)
# The final default-mode warm window includes one lower fresh-source rate; retain it explicitly.
p=SRC/'client-final-default.log'; lines=p.read_text(errors='replace').splitlines();by={};latest=None
for i,line in enumerate(lines,1):
 k=key(line)
 if not k:continue
 if (m:=iter_re.search(line)):
  latest=(k,i)
  by.setdefault(k,{}).update({'iterations':int(m[1]),'seconds':float(m[2]),'ips':float(m[3]),'new_source':int(m[4]),'line':i})
 if (m:=gpu_re.search(line)) and latest and i-latest[1]<=8:by.setdefault(latest[0],{}).update({'app_gpu_pass_ms':float(m[1])})
if (ROOT/'default-warm-windows.csv').exists():(ROOT/'default-warm-windows.csv').unlink()
with (ROOT/'default-warm-windows.csv').open('w',newline='') as f:
 w=csv.DictWriter(f,fieldnames=['timestamp','iterations','seconds','new_source_frames','fresh_per_second','app_gpu_pass_ms','source_log','source_line','source_sha256'],lineterminator='\n');w.writeheader()
 for t,x in by.items():
  if x.get('iterations',0)>=170 and 'app_gpu_pass_ms' in x:
   w.writerow({'timestamp':t,'iterations':x['iterations'],'seconds':x['seconds'],'new_source_frames':x['new_source'],'fresh_per_second':round(x['new_source']/x['seconds'],2),'app_gpu_pass_ms':x['app_gpu_pass_ms'],'source_log':p.name,'source_line':x['line'],'source_sha256':sha(p)})
# Quiet active-session q6/Zstd packet windows only; filter packet-size sentinels and stop/drain traffic.
p=SRC/'server-with-packet-telemetry.log'; lines=p.read_text(errors='replace').splitlines(); payload=[]
for i,line in enumerate(lines,1):
 if not 193<=i<=202:continue
 m=re.search(r'nxastc: stream (\d+) mean packet (\d+) bytes.*q0-q6 ([0-9/]+), raw/lz4/zstd ([0-9/]+)',line)
 if not m:continue
 q=list(map(int,m[3].split('/'))); c=list(map(int,m[4].split('/')));size=int(m[2])
 if q[6]>0 and c[2]>0 and size>1000:
  payload.append({'stream_eye':int(m[1]),'mean_packet_bytes':size,'q6_frames':q[6],'zstd_frames':c[2],'source_log':p.name,'line':i,'source_log_sha256':sha(p)})
with (ROOT/'quiet-q6-zstd-payload.csv').open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=list(payload[0]),lineterminator='\n');w.writeheader();w.writerows(payload)
summary={'stable_windows_min_iterations':170,'stable_windows_count':len(rows),'mode0_app_gpu_pass_ms':{'median':statistics.median(r['app_gpu_pass_ms'] for r in rows if r['mode']==0),'min':min(r['app_gpu_pass_ms'] for r in rows if r['mode']==0),'max':max(r['app_gpu_pass_ms'] for r in rows if r['mode']==0),'n':sum(r['mode']==0 for r in rows)},'mode2_app_gpu_pass_ms':{'median':statistics.median(r['app_gpu_pass_ms'] for r in rows if r['mode']==2),'min':min(r['app_gpu_pass_ms'] for r in rows if r['mode']==2),'max':max(r['app_gpu_pass_ms'] for r in rows if r['mode']==2),'n':sum(r['mode']==2 for r in rows)},'mode0_mode2_stable_iterations_per_second_range':[min(r['iterations_per_second'] for r in rows),max(r['iterations_per_second'] for r in rows)],'quiet_active_q6_zstd_packet_bytes_range':[min(x['mean_packet_bytes'] for x in payload),max(x['mean_packet_bytes'] for x in payload)],'quiet_active_q6_zstd_samples':len(payload),'excluded_idle_black_packet_bytes':157,'apk_sha256':'95e549812fa6ec09c04d0407bfb5b1aae23924733372b22da71be6449f2d0934','validation_file':'final-validation.json','limitations':['short stationary windows, not sustained motion proof','app own GPU pass only, not complete frame latency','no photon latency measurement']}
(ROOT/'live-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
