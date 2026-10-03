#!/usr/bin/env python3
"""Build compact, publishable figures from the private ASTC experiment."""

from __future__ import annotations

import csv
import hashlib
import json
import math
import os
import shutil
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from PIL import Image, ImageDraw, ImageFont


OUT = Path(__file__).resolve().parent
DATA = Path(os.environ.get(
    "XUASTC_SCRATCH", "/run/media/nerdrx/Lex/claude/nx-scratch/nx-xuastc-20261003"
))
THUMBS = OUT / "thumbnails"
LOGS = OUT / "evidence/logs"
LICENSES = OUT / "licenses"
for directory in (THUMBS, LOGS, LICENSES):
    directory.mkdir(parents=True, exist_ok=True)

ENCODE = json.loads((DATA / "encode-matrix-manifest.json").read_text())
ASTC_LZ4 = json.loads((DATA / "astc-lz4-results.json").read_text())
QUALITY = json.loads((DATA / "recon/quality-results.json").read_text())
QUALITY_BY_CASE = {row["config"]: row for row in QUALITY}
LZ4_BY_CASE = {row["fixture_config"]: row for row in ASTC_LZ4}
ENCODE_BY_CASE = {
    f'{row["fixture"]}-xuastc-{row["block"]}-q{row["quality"]}': row
    for row in ENCODE["cases"]
}

SOURCE_DARK = DATA / "dark-native.png"
SOURCE_FOREST = DATA / "forest-native.png"
HAAR = DATA / "haar-native-reference.png"
FONT_PATHS = (
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
)
FONT_BOLD_PATHS = (
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
)


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for path in FONT_BOLD_PATHS if bold else FONT_PATHS:
        if Path(path).exists():
            return ImageFont.truetype(path, size=size)
    return ImageFont.load_default()


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def image_rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def compare(reference: np.ndarray, candidate: np.ndarray) -> tuple[float, float]:
    delta = reference.astype(np.int16) - candidate.astype(np.int16)
    mae = float(np.abs(delta).mean())
    mse = float(np.square(delta.astype(np.float64)).mean())
    return 10 * math.log10(255**2 / mse), mae


def crop_for(fixture: str, path: Path) -> Image.Image:
    image = Image.open(path).convert("RGB")
    if fixture == "dark":
        # One duplicated eye only; this crop includes HUD text and avatar head.
        return image.crop((100, 40, 1450, 1040))
    # Independent high-frequency forest fixture: face, shirt, and foliage.
    return image.crop((250, 80, 1550, 1280))


def thumbnail(path: Path, fixture: str, output: Path) -> Image.Image:
    image = crop_for(fixture, path)
    image.thumbnail((620, 460), Image.Resampling.LANCZOS)
    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output, optimize=True)
    return image


# Verify baseline size and quality against the same native RGB fixture.
dark_native = image_rgb(SOURCE_DARK)
haar_rgb = image_rgb(HAAR)
assert dark_native.shape == haar_rgb.shape == (2176, 4352, 3)
haar_psnr, haar_mae = compare(dark_native, haar_rgb)
assert abs(haar_psnr - 34.213005) < 0.002, haar_psnr
haar_payload_bytes = 694_260  # reported from the paired-Haar probe path

# Consolidate measured fields without copying source frames into this package.
cases: list[dict] = []
for encode in ENCODE["cases"]:
    case = f'{encode["fixture"]}-xuastc-{encode["block"]}-q{encode["quality"]}'
    lz4 = LZ4_BY_CASE[case]
    quality = QUALITY_BY_CASE[case]
    rate_mbps = lz4["lz4_bytes"] * 8 * 90 / 1_000_000
    case_row = {
        "id": case,
        "fixture": encode["fixture"],
        "block": encode["block"],
        "quality": encode["quality"],
        "effort": encode["effort"],
        "profile": encode["profile"],
        "resolution": [4352, 2176],
        "ktx2_bytes": encode["ktx2_bytes"],
        "astc_block_payload_bytes": lz4["payload_bytes"],
        "lz4_payload_bytes": lz4["lz4_bytes"],
        "lz4_payload_rate_at_90fps_mbps": rate_mbps,
        "source_rgb_psnr_db": quality["source_rgb_psnr_db"],
        "source_rgb_rmse_channels": quality["channel_rmse"],
        "encode_wall_seconds_including_png_io": encode["encode_wall_seconds_including_png_io"],
        "host_astc_transcode_ms": encode["transcode_reported_ms"],
        "host_lz4_compress_ms": lz4["compress_ms"],
        "host_lz4_decompress_ms": lz4["decompress_ms"],
        "astc_sha256": lz4["astc_file_sha256"],
        "lz4_sha256": lz4["lz4_sha256"],
        "decoded_thumbnail": f'thumbnails/{case}.png',
        "decoded_thumbnail_sha256": None,
    }
    if encode["fixture"] == "dark":
        case_row["psnr_delta_vs_current_haar_db"] = quality["source_rgb_psnr_db"] - haar_psnr
    cases.append(case_row)

# Build six low-resolution, matched native/ASTC comparison thumbnails.
font_title = font(20, bold=True)
font_label = font(15)
font_small = font(13)
panel_w, panel_h, thumb_w, thumb_h = 720, 388, 334, 248
sheet = Image.new("RGB", (panel_w * 3, panel_h * 2), "#000000")
draw = ImageDraw.Draw(sheet)
sheet_cases = []
for row in cases:
    case = row["id"]
    fixture = row["fixture"]
    source_path = SOURCE_DARK if fixture == "dark" else SOURCE_FOREST
    decoded_path = DATA / "recon" / f"{case}.astc-decoded.png"
    source_crop = crop_for(fixture, source_path)
    decoded_crop = crop_for(fixture, decoded_path)
    source_crop.thumbnail((thumb_w, thumb_h), Image.Resampling.LANCZOS)
    decoded_crop.thumbnail((thumb_w, thumb_h), Image.Resampling.LANCZOS)
    thumb_path = THUMBS / f"{case}.png"
    decoded_crop.save(thumb_path, optimize=True)
    row["decoded_thumbnail_sha256"] = digest(thumb_path)
    index = len(sheet_cases)
    col, line = index % 3, index // 3
    x, y = col * panel_w, line * panel_h
    label = f'{fixture.title()} · ASTC {row["block"]} · q{row["quality"]}'
    draw.text((x + 18, y + 12), label, fill="#efeaff", font=font_title)
    draw.text((x + 18, y + 42),
              f'{row["lz4_payload_bytes"]:,} B LZ4 · {row["source_rgb_psnr_db"]:.2f} dB PSNR',
              fill="#9a8fc0", font=font_small)
    draw.text((x + 18, y + 75), "Native source", fill="#efeaff", font=font_label)
    draw.text((x + 366, y + 75), "ASTC block decode", fill="#efeaff", font=font_label)
    sheet.paste(source_crop, (x + 18, y + 103))
    sheet.paste(decoded_crop, (x + 366, y + 103))
    sheet_cases.append(case)
sheet.save(OUT / "astc-quality-contact-sheet.png", optimize=True)

# Full-resolution text-edge strip: native pixels enlarged only by nearest-neighbour.
detail_box = (350, 80, 850, 480)
detail_paths = [
    SOURCE_DARK,
    HAAR,
    DATA / "recon/dark-xuastc-8x8-q25.astc-decoded.png",
    DATA / "recon/dark-xuastc-8x8-q50.astc-decoded.png",
]
detail_labels = ["Native source", "Paired-Haar probe", "ASTC 8×8 q25", "ASTC 8×8 q50"]
zoom = 2
detail_w, detail_h = (detail_box[2] - detail_box[0]) * zoom, (detail_box[3] - detail_box[1]) * zoom
detail_header = 56
detail_strip = Image.new("RGB", (len(detail_paths) * detail_w, detail_h + detail_header), "#000000")
detail_draw = ImageDraw.Draw(detail_strip)
for i, (path, label) in enumerate(zip(detail_paths, detail_labels)):
    crop = Image.open(path).convert("RGB").crop(detail_box)
    crop = crop.resize((detail_w, detail_h), Image.Resampling.NEAREST)
    x = i * detail_w
    detail_draw.text((x + 12, 14), label + " · nearest 2×", fill="#efeaff", font=font(19, bold=True))
    detail_strip.paste(crop, (x, detail_header))
detail_strip.save(OUT / "dark-text-edge-detail-strip-2x.png", optimize=True)

# Keep source and decoded thumbnails for the leading dark crop, not original frames.
for label, image_path, fixture in (
    ("dark-native-detail", SOURCE_DARK, "dark"),
    ("haar-native-detail", HAAR, "dark"),
    ("forest-native-detail", SOURCE_FOREST, "forest"),
):
    crop = crop_for(fixture, image_path)
    crop.thumbnail((960, 720), Image.Resampling.LANCZOS)
    crop.save(THUMBS / f"{label}.png", optimize=True)

# Two-frame blink GIF is a still-image edge comparison, never motion evidence.
candidate_path = DATA / "recon/dark-xuastc-8x8-q25.astc-decoded.png"
native_crop = crop_for("dark", SOURCE_DARK)
candidate_crop = crop_for("dark", candidate_path)
gif_w = 1100
content_h = round(native_crop.height * gif_w / native_crop.width)
gif_font = font(23, bold=True)
frames = []
for image, label in ((native_crop, "NATIVE SOURCE"),
                     (candidate_crop, "ASTC 8×8 · QUALITY 25 · DECODED BLOCKS")):
    image = image.resize((gif_w, content_h), Image.Resampling.LANCZOS)
    frame = Image.new("RGB", (gif_w, content_h + 56), "#000000")
    frame.paste(image, (0, 56))
    ImageDraw.Draw(frame).text((22, 13), label, fill="#efeaff", font=gif_font)
    frames.append(frame.convert("P", palette=Image.Palette.ADAPTIVE, colors=256))
frames[0].save(OUT / "dark-source-vs-astc8-q25-blink.gif", save_all=True,
               append_images=frames[1:], duration=900, loop=0, optimize=True,
               comment=b"Two static reconstructions; not motion evidence.")

# Figure: payload/quality tradeoff and host encoder cost.
BLACK, WHITE, MUTED, VIOLET, CYAN, RED = (
    "#000000", "#efeaff", "#9a8fc0", "#7700FF", "#00e5ff", "#ff5c77"
)
plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 10,
    "text.color": WHITE, "axes.labelcolor": WHITE,
    "xtick.color": MUTED, "ytick.color": MUTED,
    "axes.edgecolor": "#463c5e", "axes.facecolor": BLACK,
    "figure.facecolor": BLACK, "savefig.facecolor": BLACK,
})
fig, (ax_quality, ax_time) = plt.subplots(
    1, 2, figsize=(15, 7.7), gridspec_kw={"width_ratios": [1.35, 1]}
)
fig.suptitle("ASTC texture path · static-frame screening", color=WHITE,
             fontsize=21, fontweight="bold", x=0.06, ha="left", y=0.975)
fig.text(0.06, 0.925,
         "4352 × 2176 duplicated stereo · PSNR vs native RGB · host CPU timings",
         color=MUTED, fontsize=11)
for ax in (ax_quality, ax_time):
    ax.grid(True, color="#463c5e", linewidth=0.7, alpha=0.58)
    ax.set_axisbelow(True)
    for spine in ax.spines.values():
        spine.set_color("#463c5e")

dark_rows = [row for row in cases if row["fixture"] == "dark"]
for block, marker, color in (("8x8", "o", VIOLET), ("12x12", "s", CYAN)):
    group = sorted((row for row in dark_rows if row["block"] == block),
                   key=lambda row: row["quality"])
    xs = [row["lz4_payload_bytes"] / 1000 for row in group]
    ys = [row["source_rgb_psnr_db"] for row in group]
    ax_quality.plot(xs, ys, color=color, linewidth=1.4, alpha=0.72)
    ax_quality.scatter(xs, ys, s=112, marker=marker, color=color,
                       edgecolor=WHITE, linewidth=0.7, label=f"ASTC {block}", zorder=5)
    for row, x, y in zip(group, xs, ys):
        ax_quality.annotate(f'q{row["quality"]}', (x, y), xytext=(8, 7),
                            textcoords="offset points", color=WHITE, fontsize=9)

haar_kb = haar_payload_bytes / 1000
ax_quality.scatter([haar_kb], [haar_psnr], marker="*", s=210, color=WHITE,
                   edgecolor="#1a1722", linewidth=0.8, label="Paired-Haar probe", zorder=6)
ax_quality.annotate("Haar baseline", (haar_kb, haar_psnr), xytext=(8, -19),
                     textcoords="offset points", color=WHITE, fontsize=9)
ax_quality.set_xlabel("Per-frame encoded payload (kB, decimal)")
ax_quality.set_ylabel("PSNR vs native dark source (dB)")
ax_quality.set_xlim(580, 1065)
ax_quality.set_ylim(30.9, 35.1)
ax_quality.set_title("A  ·  Dark fixture quality / payload", loc="left",
                     color=WHITE, fontsize=13, pad=12)
ax_quality.legend(frameon=False, labelcolor=WHITE, loc="lower right")
time_rows = sorted(cases, key=lambda row: row["encode_wall_seconds_including_png_io"])
positions = np.arange(len(time_rows))
labels = [f'{row["fixture"].title()} {row["block"]} q{row["quality"]}'
          for row in time_rows]
colors = [VIOLET if row["block"] == "8x8" else CYAN for row in time_rows]
ms = [1000 * row["encode_wall_seconds_including_png_io"] for row in time_rows]
ax_time.scatter(ms, positions, s=100, c=colors, edgecolor=WHITE,
                linewidth=0.7, zorder=5)
ax_time.set_xscale("log")
ax_time.set_xlim(5, 6000)
ax_time.set_yticks(positions, labels)
ax_time.invert_yaxis()
ax_time.axvline(1000 / 90, color=RED, linestyle=(0, (4, 3)), linewidth=1.5)
ax_time.text(18, 0.48, "90 Hz budget: 11.1 ms", color=RED, fontsize=8, va="bottom")
for x, y in zip(ms, positions):
    ax_time.annotate(f"{x:.0f} ms", (x, y), xytext=(8, 0),
                     textcoords="offset points", va="center", color=WHITE, fontsize=9)
ax_time.set_xlabel("Encode wall time (ms, logarithmic scale)")
ax_time.set_title("B  ·  Offline CPU encode cost", loc="left",
                  color=WHITE, fontsize=13, pad=12)
ax_time.tick_params(axis="y", labelsize=9)

fig.text(0.06, 0.055,
         "90 Hz rates in metrics.csv use payload bytes × 8 × 90; no transport, LZ4 framing, or packet overhead. "
         "Encoder time includes CLI startup, PNG I/O, encode, and KTX2 write. Static stills do not establish motion quality.",
         color=MUTED, fontsize=9, ha="left", va="bottom", wrap=True)
fig.subplots_adjust(left=0.075, right=0.975, top=0.82, bottom=0.19, wspace=0.32)
svg = OUT / "astc-texture-path-metrics.svg"
png = OUT / "astc-texture-path-metrics.png"
fig.savefig(svg, metadata={"Title": "ASTC texture path static-frame screening",
                           "Description": "Payload, PSNR, and CPU encode wall time; not live performance."})
fig.savefig(png, dpi=240)
plt.close(fig)

# Export compact machine-readable table.
csv_fields = ["id", "fixture", "block", "quality", "ktx2_bytes",
              "astc_block_payload_bytes", "lz4_payload_bytes",
              "lz4_payload_rate_at_90fps_mbps", "source_rgb_psnr_db",
              "encode_wall_seconds_including_png_io", "host_astc_transcode_ms",
              "host_lz4_decompress_ms", "psnr_delta_vs_current_haar_db"]
with (OUT / "metrics.csv").open("w", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=csv_fields, lineterminator="\n")
    writer.writeheader()
    for row in cases:
        writer.writerow({key: row.get(key, "") for key in csv_fields})

# Copy upstream attribution texts; generated benchmark files remain separate.
shutil.copyfile(DATA / "basis_universal/LICENSE", LICENSES / "BasisUniversal-LICENSE.txt")
shutil.copyfile(DATA / "basis_universal/NOTICE", LICENSES / "BasisUniversal-NOTICE.txt")

# Preserve short command evidence while replacing private absolute paths.
scratch_prefix = str(DATA)
for source_log in sorted((DATA / "logs").glob("*.log")):
    text = source_log.read_text(errors="replace").replace(scratch_prefix, "$XUASTC_SCRATCH")
    (LOGS / source_log.name).write_text(text)

manifest = {
    "title": "ASTC texture path, 90 Hz screening",
    "date": "2026-10-03",
    "frame": {"width": 4352, "height": 2176,
              "layout": "duplicated square view, side-by-side stereo"},
    "encoder": {"name": "Basis Universal", "version": "2.50.0",
                "commit": ENCODE["commit"], "license": "Apache-2.0",
                "profile": "XUASTC LDR, Zstd", "effort": 0,
                "blur_candidates": "disabled by default; -xuastc_blurring omitted"},
    "host": {"cpu": "AMD Ryzen 9 9950X3D 16-Core Processor",
             "os_arch": "Linux x86_64", "build": ENCODE["cmake"]},
    "source_fixtures": {
        "dark": {"sha256": ENCODE_BY_CASE["dark-xuastc-12x12-q25"]["source_sha256"],
                 "description": "Full-range BT.709 YCbCr420 upsampled to RGB8; the same view is duplicated into both eyes."},
        "forest": {"sha256": ENCODE_BY_CASE["forest-xuastc-12x12-q50"]["source_sha256"],
                   "description": "Independent VRChat forest scene; centre-cropped and duplicated to match the native frame."},
        "haar_reference": {"sha256": digest(HAAR), "bytes": HAAR.stat().st_size,
                           "packet_bytes_reported": haar_payload_bytes,
                           "rgb_psnr_vs_dark_native_db": haar_psnr,
                           "rgb_mae_vs_dark_native": haar_mae,
                           "description": "Paired-Haar probe420 host-plane frame 0, full-range BT.709, nearest UV, rounded RGB8."},
    },
    "rate_assumption": {"frames_per_second": 90,
                        "formula": "payload_bytes * 8 * 90 / 1e6",
                        "excludes": ["LZ4 frame metadata", "network framing", "headers", "transport overhead"],
                        "frame_budget_ms": 1000 / 90},
    "quality_method": {"reference": "matching native RGB8 image", "metric": "full-frame RGB PSNR",
                       "pixel_format": "sRGB RGB8", "image_metric_note": "Dark and forest PSNR values are not compared across different images."},
    "cases": cases,
    "pico_status": {
        "xuastc_cpu_full_call_p50_ms": 29.882,
        "xuastc_cpu_full_call_p95_ms": 30.461,
        "xuastc_cpu_decode_status": "Rejected; native CPU transcode alone exceeds 90 Hz interval. Retained rerun logs.",
        "lz4_astc8_two_frame_cpu_call_p50_ms": 4.70547,
        "lz4_astc8_two_frame_cpu_call_p95_ms": 5.75193,
        "lz4_astc8_two_frame_gpu_total_p50_ms": 2.53792,
        "lz4_astc8_two_frame_gpu_total_p95_ms": 3.40818,
        "standalone_lz4_cpu_p50_ms": 0.458,
        "standalone_lz4_cpu_p95_ms": 0.603,
        "scope": "Offscreen sample/write and fence, two synthetic changing images. Not live encoder, network, compositor, or photon latency.",
        "primary_trace": "evidence/pico-astc/logs/q25-alternating-samples.csv",
    },
    "generated_files": {},
}
for path in sorted(OUT.rglob("*")):
    if (path.is_file() and "build" not in path.relative_to(OUT).parts
            and path.name not in {"manifest.json", "SHA256SUMS", "REPORT-SHA256SUMS"}):
        manifest["generated_files"][str(path.relative_to(OUT))] = {
            "bytes": path.stat().st_size, "sha256": digest(path)
        }
(OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps({"output": str(OUT), "cases": len(cases),
                  "figure": str(png), "svg": str(svg),
                  "contact_sheet": str(OUT / 'astc-quality-contact-sheet.png'),
                  "blink_gif": str(OUT / 'dark-source-vs-astc8-q25-blink.gif'),
                  "haar_psnr_db": haar_psnr, "haar_mae": haar_mae}, indent=2))
