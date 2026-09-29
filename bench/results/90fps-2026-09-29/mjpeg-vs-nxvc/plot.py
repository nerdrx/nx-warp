"""Regenerate public numerical plots. No private source images required."""
from pathlib import Path
import csv
import json
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, ScalarFormatter, NullFormatter

ROOT = Path(__file__).resolve().parent
NX = json.loads((ROOT/'nx-summary.json').read_text())['fixtures']
rows = list(csv.DictReader((ROOT/'quality.csv').open()))
rows += list(csv.DictReader((ROOT/'aligned444/quality.csv').open()))
for r in rows:
    r['atlas_guard'] = int(r.get('atlas_guard', 4 if r['arm'] == 'sample-atlas' else -1))
    for k in ['quality','chroma','detail_bytes']:
        r[k] = int(r[k])
    r['wire_bytes'] = r['detail_bytes'] + NX[r['fixture']]['safety_wire_bytes']
    r['mbps90'] = r['wire_bytes']*720/1e6

plt.rcParams.update({'font.family':'DejaVu Sans', 'font.size':11,
                     'axes.spines.top':False, 'axes.spines.right':False,
                     'axes.titleweight':'bold', 'figure.facecolor':'#faf9fc',
                     'axes.facecolor':'#faf9fc', 'savefig.facecolor':'#faf9fc'})
colours = ['#7c3aed','#e0693d','#168e9c','#4d70bf']

def subset(scene,arm,q,chroma,guard=-1):
    return [r for r in rows if r['fixture'].startswith(scene) and r['arm']==arm
            and r['quality']==q and r['chroma']==chroma and r['atlas_guard']==guard]

def nx_mbps(scene):
    return np.mean([(v['selected_bytes']+v['safety_wire_bytes'])*720/1e6
                    for k,v in NX.items() if k.startswith(scene)])

fig,axes=plt.subplots(1,2,figsize=(12,5))
summary={}
for ax,scene,title in zip(axes,['forest','dark'],['Outdoor scene','Detailed indoor scene']):
    values=[nx_mbps(scene)]
    arms=[('full-raster',85,420,-1),('sample-atlas',85,444,0),('sample-atlas',75,444,0)]
    values += [np.mean([r['mbps90'] for r in subset(scene,*a)]) for a in arms]
    bars=ax.bar(range(4),values,color=colours,width=.65)
    ax.set_xticks(range(4),['NXVC\nreference','MJPEG\nQ85 4:2:0','JPEG atlas\nQ85 4:4:4','JPEG atlas\nQ75 4:4:4'])
    ax.set_title(title);ax.set_ylabel('Codec payload at 90 frames/s (Mbit/s)')
    ax.set_ylim(0,max(values)*1.18)
    for bar,value in zip(bars,values):ax.text(bar.get_x()+bar.get_width()/2,value+max(values)*.025,f'{value:.1f}',ha='center',fontweight='bold')
    ax.grid(axis='y',alpha=.15);ax.set_axisbelow(True)
    summary[scene]=dict(nxvc_mbps=values[0],mjpeg_q85_420_mbps=values[1],
                       atlas_q85_444_mbps=values[2],atlas_q75_444_mbps=values[3])
fig.suptitle('Same foveated pixels. Different payloads—and different added loss.',fontsize=16,fontweight='bold')
fig.text(.5,.025,'Mean of three source shifts per scene; both eyes duplicate one photograph. Common safety payload included.\nJPEG adds loss to the NXVC reference. These are size normalizations, not live stream measurements.',ha='center',fontsize=9)
fig.tight_layout(rect=(0,.10,1,.91));fig.savefig(ROOT/'bitrate.png',dpi=180);plt.close(fig)

fig,axes=plt.subplots(1,2,figsize=(12,5.6))
for ax,scene,title in zip(axes,['forest','dark'],['Outdoor scene','Detailed indoor scene']):
    styles=[('full-raster',420,-1,'Full raster · 4:2:0','#e0693d'),
            ('full-raster',444,-1,'Full raster · 4:4:4','#bb8525'),
            ('sample-atlas',420,4,'Guarded atlas · 4:2:0','#4d70bf'),
            ('sample-atlas',444,0,'Aligned atlas · 4:4:4','#168e9c')]
    for arm,chroma,guard,label,colour in styles:
        xs=[];ys=[]
        for q in (60,75,85,90,95,98,100):
            selected=subset(scene,arm,q,chroma,guard)
            xs.append(np.mean([r['mbps90'] for r in selected]))
            ys.append(np.mean([float(r['psnr_centre128_db']) for r in selected]))
        ax.plot(xs,ys,'o-',label=label,color=colour,markersize=4)
        if guard==0:
            for idx,q in enumerate((60,75,85,90,95,98,100)):
                if q in (75,85,95):ax.annotate(f'Q{q}',(xs[idx],ys[idx]),xytext=(5,5),textcoords='offset points',fontsize=8)
    ax.axvline(nx_mbps(scene),ls='--',color='#7c3aed',label='NXVC payload; zero added error')
    ax.set_xscale('log');ax.set_title(title);ax.grid(alpha=.18)
    ax.xaxis.set_major_locator(FixedLocator([25,50,100,200,400,800]))
    ax.xaxis.set_major_formatter(ScalarFormatter())
    ax.xaxis.set_minor_formatter(NullFormatter())
    ax.set_xlabel('Codec payload at 90 frames/s (Mbit/s, log scale)')
    ax.set_ylabel('Centre 128×128 RGB PSNR to NXVC reference (dB)')
axes[0].legend(fontsize=8,loc='lower right')
fig.suptitle('What do the saved bytes cost in the sharp centre?',fontsize=16,fontweight='bold')
fig.text(.5,.02,'Higher PSNR means less additional error; it does not establish perceptual equivalence.\nNXVC reproduces the comparison reference exactly; its PSNR is infinite, so only its payload is marked.',ha='center',fontsize=9)
fig.tight_layout(rect=(0,.10,1,.92));fig.savefig(ROOT/'rate-distortion.png',dpi=180);plt.close(fig)
(ROOT/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')

# Separate JPEG-only timing panel: all bars are CPU JPEG -> RGB with the same API.
timing=ROOT/'jpeg-decode-summary.csv'
if timing.exists():
    ts=list(csv.DictReader(timing.open()))
    fig,axes=plt.subplots(1,2,figsize=(11,4.8))
    for ax,scene,title in zip(axes,['forest-s0','dark-s0'],['Outdoor scene','Detailed indoor scene']):
        selected=[]
        for arm,guard,label in [('full-raster',4,'Full raster'),('sample-atlas',4,'Guarded atlas'),('sample-atlas',0,'Aligned atlas')]:
            r=next(t for t in ts if t['fixture']==scene and t['arm']==arm and int(t['quality'])==95 and int(t['chroma'])==444 and int(t['atlas_guard'])==guard)
            selected.append((label,float(r['p50_us'])/1000,float(r['p95_us'])/1000))
        bars=ax.bar(range(3),[t[1] for t in selected],color=['#e0693d','#4d70bf','#168e9c'])
        ax.errorbar(range(3),[t[1] for t in selected],yerr=[[0]*3,[t[2]-t[1] for t in selected]],fmt='none',ecolor='#36323f',capsize=4)
        for i,(_,v,p95) in enumerate(selected):ax.text(i,p95+.10,f'{v:.2f}',ha='center',va='bottom',fontweight='bold')
        ax.set_xticks(range(3),[t[0] for t in selected]);ax.set_title(title)
        ax.set_ylabel('Host JPEG CPU decode p50 (ms), whisker p95');ax.grid(axis='y',alpha=.15);ax.set_axisbelow(True)
    fig.suptitle('JPEG decode cost drops when repeated pixels stay packed',fontsize=15,fontweight='bold')
    fig.text(.5,.025,'Q95 4:4:4, libjpeg-turbo RGB decode; 24 measured repetitions after 12 warmups.\nExcludes atlas metadata, reconstruction, GPU upload and presentation. Host CPU timings; no Pico result.',ha='center',fontsize=9)
    fig.tight_layout(rect=(0,.12,1,.90));fig.savefig(ROOT/'jpeg-decode.png',dpi=180);plt.close(fig)
