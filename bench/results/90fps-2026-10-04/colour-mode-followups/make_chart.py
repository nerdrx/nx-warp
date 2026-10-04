#!/usr/bin/env python3
"""Render the two-cohort colour mode tradeoff chart as dependency-free SVG."""
import csv
from pathlib import Path

root = Path(__file__).resolve().parent
rows = list(csv.DictReader((root / "metrics.csv").open(newline="")))
full = [r for r in rows if r["cohort"] == "CEM6 rate-matched full image"]
crop = [r for r in rows if r["cohort"] == "CEM0 focused crop"]
W, H = 1000, 465
svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
       '<rect width="100%" height="100%" fill="#fff"/>',
       '<style>text{font-family:system-ui,sans-serif;fill:#20242a}.title{font-size:20px;font-weight:700}.sub{font-size:12px;fill:#555}.head{font-size:15px;font-weight:700}.tick{font-size:11px;fill:#555}.lab{font-size:11px}.grid{stroke:#dce1e7;stroke-width:1}.axis{stroke:#4b5563;stroke-width:1.2}</style>',
       '<text x="500" y="28" text-anchor="middle" class="title">Quality gain vs compressed growth</text>',
       '<text x="500" y="49" text-anchor="middle" class="sub">Separate cohorts and independent axes; do not read as a shared byte-budget comparison</text>']

def panel(x0, title, data, xmax, ymax, xticks, yticks, color):
    left, top, pw, ph = x0 + 48, 112, 380, 250
    svg.append(f'<text x="{x0+238}" y="82" text-anchor="middle" class="head">{title}</text>')
    for t in xticks:
        x = left + pw * t / xmax
        svg.append(f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{top+ph}" class="grid"/>')
        svg.append(f'<text x="{x:.1f}" y="{top+ph+19}" text-anchor="middle" class="tick">{t:g}</text>')
    for t in yticks:
        y = top + ph * (1 - t / ymax)
        svg.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left+pw}" y2="{y:.1f}" class="grid"/>')
        svg.append(f'<text x="{left-8}" y="{y+4:.1f}" text-anchor="end" class="tick">{t:.2f}</text>')
    svg.append(f'<line x1="{left}" y1="{top+ph}" x2="{left+pw}" y2="{top+ph}" class="axis"/>')
    svg.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top+ph}" class="axis"/>')
    svg.append(f'<text x="{left+pw/2}" y="{top+ph+42}" text-anchor="middle" class="sub">Zstd-3 growth (%)</text>')
    svg.append(f'<text x="{x0+10}" y="{top+ph/2}" text-anchor="middle" class="sub" transform="rotate(-90 {x0+10} {top+ph/2})">PSNR gain (dB)</text>')
    for r in data:
        xval=float(r['zstd3_growth_pct']); yval=float(r['gain_db'])
        px=left+pw*xval/xmax; py=top+ph*(1-yval/ymax)
        svg.append(f'<circle cx="{px:.1f}" cy="{py:.1f}" r="5" fill="{color}" stroke="#fff" stroke-width="1.5"/>')
        label=(r['image']+' q'+str(r['q']))
        svg.append(f'<text x="{px+7:.1f}" y="{py-7:.1f}" class="lab">{label}</text>')

panel(8, 'CEM6 · rate-matched full images', full, 10, .16, [0,2,4,6,8,10], [0,.04,.08,.12,.16], '#2868a5')
panel(508, 'CEM0 · forest shirt crop', crop, 50, .32, [0,10,20,30,40,50], [0,.08,.16,.24,.32], '#bb5b2c')
svg.append('<text x="500" y="454" text-anchor="middle" class="sub">Left: full-frame measurements; right: 320×320 crop. Independent cohorts, source sets and scales.</text>')
svg.append('</svg>')
(root / 'tradeoff.svg').write_text('\n'.join(svg) + '\n')
