#!/usr/bin/env python3
"""Render ``compare.py`` results as Markdown plus an SVG rate-distortion plot.

Writes into ``tools/quality/reports/`` by default::

    python3 report.py --results $NXQ_SCRATCH/results/dummy444.json

Several result files can be given at once; each becomes a section of one
report, with its own RD table and plot.

The plot is drawn with matplotlib (Agg backend, never a GUI window) when it is
installed, and by a small hand-written SVG writer otherwise, so the report
never silently loses its figure on a machine without matplotlib.
"""

from __future__ import annotations

import argparse
import datetime
import html
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

REPORTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "reports")

# A colour-blind-safe qualitative set; the codec under test is always the first.
PALETTE = ["#7700ff", "#e66100", "#1a85ff", "#117733", "#d41159", "#994f00"]


# --- plotting ------------------------------------------------------------


def _series(results: dict, metric: str) -> list[dict]:
    """Ordered series for plotting: the codec first, then the anchors."""
    ck = results["codec_key"]
    out = []
    for name in [ck] + sorted(n for n in results["codecs"] if n != ck):
        entry = results["codecs"].get(name, {})
        pts = [
            p for p in entry.get("points", [])
            if metric in p and p[metric] is not None
            and isinstance(p[metric], (int, float)) and math.isfinite(p[metric])
        ]
        if not pts:
            continue
        pts = sorted(pts, key=lambda p: p["bitrate_mbps"])
        out.append({
            "name": name,
            "is_codec": name == ck,
            "x": [p["bitrate_mbps"] for p in pts],
            "y": [float(p[metric]) for p in pts],
            "labels": [str(p.get("qp", p.get("crf", ""))) for p in pts],
        })
    return out


METRIC_LABEL = {
    "psnr_y": "PSNR-Y (dB)",
    "psnr_ycbcr": "PSNR-YCbCr, (6Y+Cb+Cr)/8 (dB)",
    "ssim_y": "SSIM (Y)",
    "ms_ssim_y": "MS-SSIM (Y)",
    "vmaf": "VMAF",
}


def plot_matplotlib(series: list[dict], metric: str, title: str, path: str) -> bool:
    if not series:
        return False  # decline empty input, like plot_svg, instead of drawing blank axes
    try:
        import matplotlib
        matplotlib.use("Agg")  # never open a window
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    fig, ax = plt.subplots(figsize=(7.2, 4.6), dpi=100)
    for i, s in enumerate(series):
        ax.plot(
            s["x"], s["y"],
            marker="o" if s["is_codec"] else "s",
            markersize=6 if s["is_codec"] else 4.5,
            linewidth=2.4 if s["is_codec"] else 1.5,
            color=PALETTE[i % len(PALETTE)],
            label=s["name"] + (" (codec)" if s["is_codec"] else ""),
            zorder=3 if s["is_codec"] else 2,
        )
        for x, y, lab in zip(s["x"], s["y"], s["labels"]):
            ax.annotate(lab, (x, y), textcoords="offset points", xytext=(4, -10),
                        fontsize=6.5, color=PALETTE[i % len(PALETTE)], alpha=0.85)
    ax.set_xscale("log")
    ax.set_xlabel("bitrate (Mbit/s, log scale)")
    ax.set_ylabel(METRIC_LABEL.get(metric, metric))
    ax.set_title(title, fontsize=10)
    ax.grid(True, which="both", alpha=0.25, linewidth=0.6)
    ax.legend(fontsize=8, loc="lower right")
    fig.tight_layout()
    fig.savefig(path, format="svg")
    plt.close(fig)
    return True


def plot_svg(series: list[dict], metric: str, title: str, path: str) -> bool:
    """Hand-written SVG fallback: no dependencies at all."""
    if not series:
        return False
    W, H = 720, 460
    ml, mr, mt, mb = 68, 150, 40, 52
    pw, ph = W - ml - mr, H - mt - mb
    xs = [x for s in series for x in s["x"]]
    ys = [y for s in series for y in s["y"]]
    if not xs or min(xs) <= 0:
        return False
    lx0, lx1 = math.log10(min(xs)), math.log10(max(xs))
    if lx1 - lx0 < 1e-9:
        lx0, lx1 = lx0 - 0.5, lx1 + 0.5
    y0, y1 = min(ys), max(ys)
    if y1 - y0 < 1e-9:
        y0, y1 = y0 - 1, y1 + 1
    pad = (y1 - y0) * 0.08
    y0, y1 = y0 - pad, y1 + pad

    def px(v: float) -> float:
        return ml + (math.log10(v) - lx0) / (lx1 - lx0) * pw

    def py(v: float) -> float:
        return mt + ph - (v - y0) / (y1 - y0) * ph

    o = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
        f'viewBox="0 0 {W} {H}" font-family="system-ui,sans-serif">',
        f'<rect width="{W}" height="{H}" fill="#ffffff"/>',
        f'<text x="{W / 2:.0f}" y="22" text-anchor="middle" font-size="13" '
        f'fill="#111">{html.escape(title)}</text>',
    ]
    # grid + axes
    for i in range(6):
        gy = mt + ph * i / 5
        val = y1 - (y1 - y0) * i / 5
        o.append(f'<line x1="{ml}" y1="{gy:.1f}" x2="{ml + pw}" y2="{gy:.1f}" '
                 f'stroke="#e3e3e8" stroke-width="1"/>')
        o.append(f'<text x="{ml - 8}" y="{gy + 4:.1f}" text-anchor="end" font-size="10" '
                 f'fill="#555">{val:.3g}</text>')
    decade0, decade1 = math.floor(lx0), math.ceil(lx1)
    d = decade0
    while d <= decade1:
        for m in range(1, 10):
            v = m * (10.0**d)
            if not (min(xs) <= v <= max(xs)):
                continue
            gx = px(v)
            major = m == 1
            o.append(f'<line x1="{gx:.1f}" y1="{mt}" x2="{gx:.1f}" y2="{mt + ph}" '
                     f'stroke="{"#d8d8de" if major else "#f0f0f3"}" stroke-width="1"/>')
            if major or m in (2, 5):
                o.append(f'<text x="{gx:.1f}" y="{mt + ph + 16}" text-anchor="middle" '
                         f'font-size="10" fill="#555">{v:g}</text>')
        d += 1
    o.append(f'<rect x="{ml}" y="{mt}" width="{pw}" height="{ph}" fill="none" '
             f'stroke="#999" stroke-width="1"/>')
    o.append(f'<text x="{ml + pw / 2:.0f}" y="{H - 10}" text-anchor="middle" font-size="11" '
             f'fill="#333">bitrate (Mbit/s, log scale)</text>')
    o.append(f'<text x="16" y="{mt + ph / 2:.0f}" text-anchor="middle" font-size="11" '
             f'fill="#333" transform="rotate(-90 16 {mt + ph / 2:.0f})">'
             f'{html.escape(METRIC_LABEL.get(metric, metric))}</text>')

    for i, s in enumerate(series):
        c = PALETTE[i % len(PALETTE)]
        pts = " ".join(f"{px(x):.1f},{py(y):.1f}" for x, y in zip(s["x"], s["y"]))
        o.append(f'<polyline points="{pts}" fill="none" stroke="{c}" '
                 f'stroke-width="{2.6 if s["is_codec"] else 1.6}"/>')
        for x, y, lab in zip(s["x"], s["y"], s["labels"]):
            o.append(f'<circle cx="{px(x):.1f}" cy="{py(y):.1f}" '
                     f'r="{4.2 if s["is_codec"] else 3.2}" fill="{c}"/>')
            o.append(f'<text x="{px(x) + 5:.1f}" y="{py(y) + 13:.1f}" font-size="8" '
                     f'fill="{c}" opacity="0.85">{html.escape(lab)}</text>')
        ly = mt + 8 + i * 18
        o.append(f'<line x1="{ml + pw + 14}" y1="{ly}" x2="{ml + pw + 34}" y2="{ly}" '
                 f'stroke="{c}" stroke-width="3"/>')
        name = s["name"] + (" (codec)" if s["is_codec"] else "")
        o.append(f'<text x="{ml + pw + 39}" y="{ly + 4}" font-size="10" fill="#222">'
                 f'{html.escape(name)}</text>')
    o.append("</svg>")
    with open(path, "w") as fh:
        fh.write("\n".join(o))
    return True


def write_plot(series: list[dict], metric: str, title: str, path: str, prefer: str = "auto") -> str:
    """Write the RD plot; returns the backend actually used."""
    if prefer in ("auto", "matplotlib") and plot_matplotlib(series, metric, title, path):
        return "matplotlib"
    if plot_svg(series, metric, title, path):
        return "svg"
    return "none"


# --- markdown ------------------------------------------------------------


def _fmt(v, spec="{:.3f}", dash="-"):
    if v is None or (isinstance(v, float) and not math.isfinite(v)):
        return "lossless" if isinstance(v, float) and v == float("inf") else dash
    if isinstance(v, (int, float)):
        return spec.format(v)
    return str(v)


def rd_table(entry: dict, name: str) -> list[str]:
    pts = entry.get("points", [])
    if not pts:
        return [f"_{name}: no operating points_ "
                f"({entry.get('reason', 'encoder failed or unavailable')})", ""]
    rc = entry.get("rate_control", "qp")
    has_ssim = any("ssim_y" in p for p in pts)
    has_ms = any("ms_ssim_y" in p for p in pts)
    has_vmaf = any("vmaf" in p for p in pts)
    head = [rc.upper(), "Mbit/s", "kB/frame", "PSNR-Y", "PSNR-YCbCr"]
    if has_ssim:
        head.append("SSIM")
    if has_ms:
        head.append("MS-SSIM")
    if has_vmaf:
        head.append("VMAF")
    rows = ["| " + " | ".join(head) + " |",
            "|" + "|".join(["---:"] * len(head)) + "|"]
    frames = max(1, pts[0].get("frames", 1))
    for p in sorted(pts, key=lambda q: q["bitrate_mbps"]):
        cells = [
            str(p.get(rc, "")),
            _fmt(p["bitrate_mbps"], "{:.2f}"),
            _fmt(p["bytes"] / max(1, p.get("frames", frames)) / 1000.0, "{:.1f}"),
            _fmt(p.get("psnr_y"), "{:.2f}"),
            _fmt(p.get("psnr_ycbcr"), "{:.2f}"),
        ]
        if has_ssim:
            cells.append(_fmt(p.get("ssim_y"), "{:.4f}"))
        if has_ms:
            cells.append(_fmt(p.get("ms_ssim_y"), "{:.4f}"))
        if has_vmaf:
            cells.append(_fmt(p.get("vmaf"), "{:.2f}"))
        rows.append("| " + " | ".join(cells) + " |")
    rows.append("")
    return rows


def section(results: dict, plot_rel: str | None, metric: str) -> list[str]:
    seq = results["sequence"]
    ck = results["codec_key"]
    md: list[str] = []
    md.append(f"## {seq['name']}")
    md.append("")
    md.append(f"- **Source**: `{seq['source']}`")
    md.append(f"- **Geometry**: {seq['width']}x{seq['height']} {seq['pix_fmt']}, "
              f"{seq['frames']} frames at {seq['fps']:g} fps, layout `{seq.get('layout', '?')}`")
    m = results.get("machine", {})
    md.append(f"- **ffmpeg**: {m.get('ffmpeg') or 'not available'}; "
              f"encoders: {', '.join(m.get('encoders') or ['none'])}; "
              f"VMAF: {'yes' if m.get('vmaf') else 'no'}")
    md.append(f"- **Run**: {results.get('generated', '?')} on `{m.get('host', '?')}`, "
              f"{results.get('elapsed_s', 0):.1f} s")
    md.append("")

    if plot_rel:
        md.append(f"![RD curves for {seq['name']}]({plot_rel})")
        md.append("")

    md.append("### Rate-distortion points")
    md.append("")
    for name in [ck] + sorted(n for n in results["codecs"] if n != ck):
        entry = results["codecs"][name]
        kind = "codec under test" if name == ck else "anchor"
        md.append(f"**{name}** ({kind})")
        md.append("")
        md += rd_table(entry, name)

    md.append("### BD-rate")
    md.append("")
    md.append(f"Bjontegaard delta rate of `{ck}` against each anchor. "
              "Negative BD-rate means fewer bits at matched quality (better); "
              "positive BD-PSNR means more quality at matched rate (better).")
    md.append("")
    for metric_key, table in results.get("bd_rate", {}).items():
        if not table:
            continue
        md.append(f"On **{METRIC_LABEL.get(metric_key, metric_key)}**:")
        md.append("")
        # BD-PSNR is only "PSNR" when the fitted distortion is PSNR; on SSIM the
        # same integral is a mean SSIM difference, so label the column honestly.
        dcol = "BD-PSNR (dB)" if metric_key.startswith("psnr") else \
            f"BD-quality ({METRIC_LABEL.get(metric_key, metric_key)})"
        md.append(f"| anchor | BD-rate (%) | {dcol} | overlap | method |")
        md.append("|---|---:|---:|---|---|")
        for anchor, bd in table.items():
            if "error" in bd:
                md.append(f"| {anchor} | n/a | n/a | {bd['error']} | {bd.get('method', '')} |")
            else:
                md.append(f"| {anchor} | {bd['bd_rate_pct']:+.2f} | {bd['bd_psnr_db']:+.3f} | "
                          f"{bd['overlap_lo']:.3f} to {bd['overlap_hi']:.3f} | {bd['method']} |")
        md.append("")

    p1 = results.get("phase1", {})
    md.append("### Phase 1 exit criterion")
    md.append("")
    md.append("> PAPER.md 3.11: *within 1.0 dB PSNR of x264 intra (`--keyint 1`, zerolatency) "
              "at 100 to 400 Mbit on VR captures*")
    md.append("")
    if "error" in p1:
        md.append(f"**Not evaluated.** {p1['error']}")
    elif p1:
        verdict = "**PASS**" if p1.get("pass") else "**FAIL**"
        md.append(f"{verdict} against `{p1['anchor']}` over "
                  f"{p1['covered_mbps'][0]:.1f} to {p1['covered_mbps'][1]:.1f} Mbit/s "
                  f"(the part of the {p1['band_mbps'][0]:.0f}-{p1['band_mbps'][1]:.0f} Mbit band "
                  "both curves cover):")
        md.append("")
        md.append("| | dB |")
        md.append("|---|---:|")
        md.append(f"| worst delta (at {p1['worst_at_mbps']:.1f} Mbit/s) | "
                  f"{p1['worst_delta_db']:+.3f} |")
        md.append(f"| mean delta | {p1['mean_delta_db']:+.3f} |")
        md.append(f"| best delta | {p1['best_delta_db']:+.3f} |")
        md.append(f"| tolerance | {-p1['tolerance_db']:+.3f} |")
    md.append("")

    vs = results.get("velocity_split")
    if vs and "error" not in vs:
        md.append("### Angular-velocity split")
        md.append("")
        md.append(f"PAPER.md 2.11 item 1 asks for results on the "
                  f"{vs['percentile']:.0f} percent of frames with the highest angular velocity. "
                  f"Threshold {vs['threshold_deg_s']:.1f} deg/s, "
                  f"{vs['high_frames']} of {vs['total_frames']} frames.")
        md.append("")
        md.append("| codec | point | Mbit/s | PSNR-Y high velocity | PSNR-Y rest | delta |")
        md.append("|---|---:|---:|---:|---:|---:|")
        for name, pts in vs.get("codecs", {}).items():
            for p in pts:
                hi, lo = p["psnr_y_high_velocity"], p["psnr_y_low_velocity"]
                delta = f"{hi - lo:+.2f}" if (hi is not None and lo is not None) else "-"
                md.append(f"| {name} | {p['rate_control']} | {p['bitrate_mbps']:.2f} | "
                          f"{_fmt(hi, '{:.2f}')} | {_fmt(lo, '{:.2f}')} | {delta} |")
        md.append("")
    return md


def build(result_paths: list[str], out_md: str, metric: str, backend: str, title: str) -> str:
    outdir = os.path.dirname(os.path.abspath(out_md))
    os.makedirs(outdir, exist_ok=True)
    md = [f"# {title}", ""]
    md.append(f"Generated {datetime.datetime.now().astimezone().isoformat(timespec='seconds')} "
              f"by `tools/quality/report.py`.")
    md.append("")
    md.append("Rate is the mean bitrate of the coded elementary stream at the sequence's "
              "frame rate (no container overhead). Quality metrics are computed by "
              "`tools/quality/nxq/metrics.py` in numpy; VMAF comes from ffmpeg's libvmaf.")
    md.append("")

    used = []
    for rp in result_paths:
        with open(rp) as fh:
            results = json.load(fh)
        series = _series(results, metric)
        plot_rel = None
        if series:
            base = os.path.splitext(os.path.basename(out_md))[0]
            svg_name = f"{base}-{results['sequence']['name']}-{metric}.svg".replace("/", "_")
            svg_path = os.path.join(outdir, svg_name)
            b = write_plot(series, metric, f"{results['sequence']['name']}: rate vs "
                                           f"{METRIC_LABEL.get(metric, metric)}",
                           svg_path, backend)
            if b != "none":
                plot_rel = svg_name
                used.append(b)
        md += section(results, plot_rel, metric)
        md.append("---")
        md.append("")

    if used:
        md.append(f"_Plots rendered with {', '.join(sorted(set(used)))}._")
        md.append("")
    with open(out_md, "w") as fh:
        fh.write("\n".join(md))
    return out_md


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", nargs="+", required=True, help="one or more compare.py JSON files")
    ap.add_argument("--out", default=None, help="output Markdown path (default: reports/<name>.md)")
    ap.add_argument("--metric", default="psnr_y",
                    choices=sorted(METRIC_LABEL), help="metric on the RD plot")
    ap.add_argument("--backend", default="auto", choices=("auto", "matplotlib", "svg"))
    ap.add_argument("--title", default="NX Warp quality report")
    args = ap.parse_args(argv)

    for p in args.results:
        if not os.path.exists(p):
            ap.error(f"no such results file: {p}")

    out = args.out
    if not out:
        stem = os.path.splitext(os.path.basename(args.results[0]))[0]
        out = os.path.join(REPORTS_DIR, f"{stem}.md")
    path = build(args.results, out, args.metric, args.backend, args.title)
    print(f"[report] wrote {path}")
    for f in sorted(os.listdir(os.path.dirname(os.path.abspath(path)))):
        if f.endswith(".svg") and f.startswith(os.path.splitext(os.path.basename(path))[0]):
            print(f"[report] wrote {os.path.join(os.path.dirname(path), f)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
