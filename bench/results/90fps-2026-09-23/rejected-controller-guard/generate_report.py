#!/usr/bin/env python3
"""Generate the public rejected-experiment report from sanitized events."""
import json
import re
from pathlib import Path


def svg(events, out):
    points = [(500.0, "start")]
    for event in events:
        m = re.search(r"(\d+\.\d+) -> (\d+\.\d+) Mbit/s", event["event"])
        if m:
            points.append((float(m[2]), event["event"].split(",")[0]))
    width, height, left, top, bottom = 760, 360, 70, 35, 60
    max_y = 550
    plot_h = height - top - bottom
    step = (width - left - 25) / max(1, len(points) - 1)
    out_lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        '<style>text{font:13px sans-serif;fill:#202124}.grid{stroke:#ddd}.line{fill:none;stroke:#d33;stroke-width:3}.axis{stroke:#555}</style>',
        '<text x="20" y="22" font-weight="bold">Rejected guard soak: bitrate transitions</text>',
    ]
    for tick in range(0, 551, 100):
        y = top + plot_h - tick / max_y * plot_h
        out_lines += [f'<line class="grid" x1="{left}" y1="{y:.1f}" x2="{width-25}" y2="{y:.1f}"/>',
                      f'<text x="{left-8}" y="{y+4:.1f}" text-anchor="end">{tick}</text>']
    out_lines += [f'<line class="axis" x1="{left}" y1="{top}" x2="{left}" y2="{top+plot_h}"/>',
                  f'<line class="axis" x1="{left}" y1="{top+plot_h}" x2="{width-25}" y2="{top+plot_h}"/>']
    coords=[]
    for i,(value,_) in enumerate(points):
        coords.append((left+i*step, top+plot_h-value/max_y*plot_h))
    out_lines.append('<polyline class="line" points="'+' '.join(f'{x:.1f},{y:.1f}' for x,y in coords)+'"/>')
    for (value,label),(x,y) in zip(points,coords):
        out_lines.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4" fill="#d33"/>')
        out_lines.append(f'<text x="{x:.1f}" y="{y-9:.1f}" text-anchor="middle">{value:.1f}</text>')
    out_lines.append('<text x="380" y="350" text-anchor="middle">event order, not wall-clock scale</text></svg>')
    out.write_text('\n'.join(out_lines)+'\n')


def main():
    root=Path(__file__).parent
    data=json.loads((root/'events.json').read_text())
    transitions=data['controller_events']
    table=['| Event line | Controller event |','|---:|---|']
    table += [f'| {x["line"]} | {x["event"]} |' for x in transitions]
    text='\n'.join([
        '# Rejected controller guard experiment',
        '',
        '**Disposition: rejected for live promotion.** The unit fix was correct in isolation, but the live soak was aborted after about four minutes because quality stayed stuck after the early backoff.',
        '',
        'This report contains controller telemetry only. It has no source photos and makes no perceptual or full-session performance claim.',
        '',
        '## Configuration',
        '',
        'The run used a 500 Mbit/s ceiling, BBR v2, native RGB888, 64 tail packets, and compression credit disabled. The guard was reverted at commit `29f74462`; existing congestion and loss logic was preserved.',
        '',
        '## Evidence',
        '',
        *table,
        '',
        'The controller backed off from 500.0 to 245.0 Mbit/s on p90 utilization 1.03 with 11 late frames, recovered only to 297.5 Mbit/s, briefly probed to 385.0, then returned to 297.5. It later dropped the bandwidth estimate after 10 seconds without a loaded frame.',
        '',
        '![Bitrate transitions](events.svg)',
        '',
        f'Unit validation remained green: {data["unit_validation"]["bbr_checks"]} BBR checks passed, and the ASAN/UBSAN run passed {data["unit_validation"]["asan_ubsan_checks"]} checks with no diagnostics. Those tests do not override the live quality result.',
        '',
        'The soak was stopped intentionally once the quality target was clearly not recovering; it is incomplete evidence, not a 20-minute soak result.',
        '',
        'Regenerate with `python3 generate_report.py` from this directory.',
    ])+'\n'
    (root/'README.md').write_text(text)
    svg(transitions,root/'events.svg')


if __name__=='__main__': main()
