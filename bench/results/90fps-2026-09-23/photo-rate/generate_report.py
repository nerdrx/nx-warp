import json
from pathlib import Path

def main():
 p=Path(__file__).parent; d=json.loads((p/'data.json').read_text()); rows=[]
 for k,v in d['cases'].items():
  s,c=v['server'],v['client']; rows.append({'requested':v['requested_mbps'],'payload':s['payload_mbps']['mean'],'payload_p95':s['payload_mbps']['p95'],'encode':s['encode_ms']['mean'],'source':c.get('new_source_fps',{}).get('mean'),'fps':s['encoder_fps']['mean'],'server_windows':s['retained_windows'],'client_windows':c.get('retained_windows',0),'network_holes':v.get('network_holes',0)})
 rows.sort(key=lambda r:r['requested']);
 table=['| Requested Mbit/s | Payload Mbit/s | Encode ms | Fresh source fps | Encoder fps | Server/Client windows | Network holes |','|---:|---:|---:|---:|---:|---:|---:|']+[f"| {r['requested']} | {r['payload']:.1f} (p95 {r['payload_p95']:.1f}) | {r['encode']:.2f} | {r['source']:.1f} | {r['fps']:.1f} | {r['server_windows']}/{r['client_windows']} | {r['network_holes']} |" for r in rows]
 (p/'summary.json').write_text(json.dumps({'source':'data.json','rows':rows,'p95':'nearest-rank p95 of retained server window means'},indent=2)+'\n')
 (p/'README.md').write_text('\n'.join(['# Photo rate sweep','', 'Fixed crowd scene, Zstd level 3, native RGB888, tail64, newest selection, compression credit off. Each requested rate ran 50 seconds after source upload. This measures requested encoder budget against actual compressed payload; fresh-source FPS is shown separately from encoder/render-loop FPS. It is a synthetic shifted-photo throughput smoke test, not a perceptual or game-quality proof.','',*table,'','The report retains the complete server/client window counts shown in the table (the 50-second runs usually yield 20 server windows after warmup; the 500 case has 21 server windows). Network holes are reported explicitly, including nonzero cases; values report the mean plus nearest-rank p95 of window means. No private paths or source photos are bundled.','', '![Requested rate versus payload](comparison.svg)','', 'Regenerate with `python3 generate_report.py`.'])+'\n')
 w,h=760,380; left,top,bottom=70,30,300; maxy=120; pts=[]
 for i,r in enumerate(rows): pts.append((left+(w-left-30)*(r['requested']/500),top+(bottom-top)*(1-r['payload']/maxy)))
 svg=['<svg xmlns="http://www.w3.org/2000/svg" width="760" height="380"><rect width="100%" height="100%" fill="white"/><style>text{font:14px sans-serif;fill:#202124}.grid{stroke:#ddd}.line{stroke:#7700ff;stroke-width:3;fill:none}</style><text x="20" y="22" font-weight="bold">Requested rate versus actual payload</text>']
 for y in range(0,121,20):
  yy=top+(bottom-top)*(1-y/maxy); svg += [f'<line class="grid" x1="{left}" y1="{yy:.1f}" x2="740" y2="{yy:.1f}"/><text x="{left-8}" y="{yy+4:.1f}" text-anchor="end">{y}</text>']
 svg += [f'<line class="grid" x1="{left}" y1="{bottom}" x2="740" y2="{bottom}"/>']
 for tick in range(0,501,100):
  xx=left+(w-left-30)*(tick/500); svg.append(f'<text x="{xx:.1f}" y="325" text-anchor="middle">{tick}</text>')
 svg+=['<polyline class="line" points="'+' '.join(f'{x:.1f},{y:.1f}' for x,y in pts)+'"/>']
 for r,(x,y) in zip(rows,pts): svg += [f'<circle cx="{x:.1f}" cy="{y:.1f}" r="5" fill="#7700ff"/><text x="{x+6 if r["requested"] == 25 else x:.1f}" y="{y-10:.1f}" text-anchor="{"start" if r["requested"] == 25 else "middle"}">{r["payload"]:.1f}</text>']
 svg.append('<text x="380" y="370" text-anchor="middle">Requested rate (Mbit/s)</text><text x="18" y="165" transform="rotate(-90 18 165)" text-anchor="middle">Actual payload (Mbit/s)</text></svg>'); (p/'comparison.svg').write_text('\n'.join(svg)+'\n')
if __name__=='__main__': main()
