#!/usr/bin/env python3
"""Generate the public lossless photo bench report from analyzer JSON."""
import json
import math
import sys
from pathlib import Path


def f(stats, key):
    return stats[key]["mean"]


def p(stats, key):
    return stats[key]["p95"]


def esc(value):
    return (str(value).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def svg(rows, out):
    width, height = 860, 390
    left, top, bottom = 150, 35, 55
    plot_h = height - top - bottom
    max_value = max(r["payload"] for r in rows) * 1.12
    bar_w = 70
    gap = 32
    x0 = left
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#fff"/>',
        '<style>text{font:14px sans-serif;fill:#202124}.axis{stroke:#555}.grid{stroke:#ddd}.lz4{fill:#7b61ff}.zstd{fill:#00a6a6}</style>',
        '<text x="20" y="22" font-weight="bold">Mean payload at 90 fps (Mbit/s)</text>',
    ]
    for tick in range(0, int(max_value) + 1, 50):
        y = top + plot_h - (tick / max_value) * plot_h
        parts.append(f'<line class="grid" x1="{left}" y1="{y:.1f}" x2="{width-20}" y2="{y:.1f}"/>')
        parts.append(f'<text x="{left-10}" y="{y+5:.1f}" text-anchor="end">{tick}</text>')
    parts.append(f'<line class="axis" x1="{left}" y1="{top}" x2="{left}" y2="{top+plot_h}"/>')
    parts.append(f'<line class="axis" x1="{left}" y1="{top+plot_h}" x2="{width-20}" y2="{top+plot_h}"/>')
    for i, row in enumerate(rows):
        x = x0 + i * (bar_w + gap)
        h = row["payload"] / max_value * plot_h
        y = top + plot_h - h
        cls = "zstd" if row["codec"] == "Zstd" else "lz4"
        parts.append(f'<rect class="{cls}" x="{x}" y="{y:.1f}" width="{bar_w}" height="{h:.1f}"/>')
        parts.append(f'<text x="{x+bar_w/2:.1f}" y="{y-6:.1f}" text-anchor="middle">{row["payload"]:.1f}</text>')
        parts.append(f'<text x="{x+bar_w/2:.1f}" y="{top+plot_h+20}" text-anchor="middle">{esc(row["scene"])}</text>')
        parts.append(f'<text x="{x+bar_w/2:.1f}" y="{top+plot_h+37}" text-anchor="middle">{row["codec"]}</text>')
    parts.append('</svg>')
    out.write_text("\n".join(parts) + "\n")


def png(rows, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    labels = [f'{r["scene"]}\n{r["codec"]}' for r in rows]
    values = [r["payload"] for r in rows]
    colors = ["#7b61ff" if r["codec"] == "LZ4" else "#00a6a6" for r in rows]
    figure, axis = plt.subplots(figsize=(8.6, 3.9), dpi=200)
    axis.bar(labels, values, color=colors)
    axis.set_ylabel("Mean payload (Mbit/s)")
    axis.set_title("Mean payload at 90 fps")
    axis.grid(axis="y", color="#ddd")
    axis.set_axisbelow(True)
    for index, value in enumerate(values):
        axis.text(index, value + 3, f"{value:.1f}", ha="center", fontsize=8)
    figure.tight_layout()
    figure.savefig(out)
    plt.close(figure)


def main():
    if len(sys.argv) == 1:
        source, out = Path(__file__).with_name("windows.json"), Path(__file__).parent
    elif len(sys.argv) == 3:
        source, out = Path(sys.argv[1]), Path(sys.argv[2])
    else:
        raise SystemExit("usage: generate_report.py [ANALYSIS_JSON OUTPUT_DIR]")
    data = json.loads(source.read_text())
    out.mkdir(parents=True, exist_ok=True)
    if not data.get("complete"):
        raise SystemExit("analysis JSON is incomplete")

    rows = []
    for scene in ("crowd", "forest"):
        for suffix, codec in (("zstd0", "LZ4"), ("zstd1", "Zstd")):
            case = data["cases"][f"{scene}-{suffix}"]
            s, c = case["server"], case["client"]
            rows.append({"scene": scene, "codec": codec,
                         "fps": f(s, "encoder_fps"), "fps_p95": p(s, "encoder_fps"),
                         "payload": f(s, "payload_mbps"), "payload_p95": p(s, "payload_mbps"),
                         "encode": f(s, "encode_ms"), "decode": f(c, "decode_ms"),
                         "decode_p95": p(c, "decode_ms"), "render": f(c, "renderloop_fps"),
                         "source": f(c, "new_source_fps"),
                         "windows": s["retained_windows"]})

    def row(scene, codec):
        return next(x for x in rows if x["scene"] == scene and x["codec"] == codec)

    table = [
        "| Scene | Codec | Encoder fps | Payload Mbit/s | Encode ms | Source fps | Pico app-loop fps | Pico decode ms | Windows |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for r in rows:
        table.append(f'| {r["scene"]} | {r["codec"]} | {r["fps"]:.1f} (p95 window mean {r["fps_p95"]:.1f}) | {r["payload"]:.1f} (p95 window mean {r["payload_p95"]:.1f}) | {r["encode"]:.2f} | {r["source"]:.1f} | {r["render"]:.1f} | {r["decode"]:.1f} (p95 window mean {r["decode_p95"]:.1f}) | {r["windows"]} |')

    reductions = []
    for scene in ("crowd", "forest"):
        a, b = row(scene, "LZ4"), row(scene, "Zstd")
        reductions.append((scene, (a["payload"] - b["payload"]) / a["payload"] * 100))

    summary = [
        "# 90 fps live lossless photo bench",
        "",
        "This is a paired server/Pico throughput smoke test using synthetic photo scenes with a shifted source frame. It measures transport and codec cost; it is not a game workload, motion comfort result, or perceptual quality proof.",
        "",
        "The run used native RGB888 capture, fixed 500 Mbps probing, compression credit disabled, and 64 tail packets in both codec modes. Each row is a full-frame lossless envelope, so the codec comparison does not introduce a quality tradeoff. Fixture byte identity is a separate proof and is intentionally not inferred from these throughput logs.",
        "",
        "## Results",
        "",
        *table,
        "",
        "Zstd reduces mean payload from 152.6 to 103.5 Mbit/s on crowd (32.2%) and from 76.5 to 52.8 Mbit/s on forest (31.0%), while both modes sustain about 90 fps. Pico decode rises from 0.2 to 0.6 ms for crowd and from 0.2 to 0.4 ms for forest.",
        "",
        "![Mean payload by scene and codec](comparison.png)",
        "",
        "## Numeric window policy",
        "",
        "The analyzer retained 20 complete two-second windows per case and discarded the first five server windows as warmup. The table reports means and nearest-rank p95 values of those window means, never frame-level p95 values. The bundled sanitized windows are the reproducible source for the numbers; regenerate this report with:",
        "",
        "```text",
        "python3 generate_report.py",
        "```",
        "",
        f"Source analysis generated at {data['generated_utc']}.",
        "",
        "The report contains no source images or private log paths.",
    ]
    (out / "README.md").write_text("\n".join(summary) + "\n")
    svg(rows, out / "payload.svg")
    png(rows, out / "comparison.png")
    (out / "summary.json").write_text(json.dumps({
        "source": "windows.json",
        "window_policy": {"server_warmup_discarded": 5, "retained_complete_windows": 20,
                           "p95": "nearest-rank p95 of retained window means"},
        "rows": rows,
        "reductions_percent": {scene: value for scene, value in reductions},
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
