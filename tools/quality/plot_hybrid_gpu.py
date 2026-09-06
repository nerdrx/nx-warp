#!/usr/bin/env python3
"""The hybrid-versus-nxvc figure for ADR-0030.

Two panels over the same six vrroom trajectories: what a hardware-HEVC base layer costs
the link, and what it saves the headset's Adreno. Palette, the direct-labelling and the
identity-is-never-colour-alone rule are tools/quality/plot_vrroom.py's.

  python3 tools/quality/plot_hybrid_gpu.py --in nx-scratch/hybgpu/results.json --out docs/assets
"""
import argparse, json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#dcdbd6"
C_TODAY = "#2a78d6"
C_TODAY_L = "#a9c6ea"
C_HYB = "#eb6834"
C_BASE = "#f6c9a8"
TRAJ = ["still", "rest", "objmotion-still", "objmotion", "mid", "fast"]
LABEL = {"still": "still", "rest": "rest", "objmotion-still": "obj motion,\nhead still",
         "objmotion": "obj motion", "mid": "mid", "fast": "fast"}
TILES = 578
# docs/NXWARP-HYBRID.md 3.2 and ADR-0029, used only as a ratio.
US_CODED, US_SKIP, US_BASE = 41.0, 34.0, 1.9


def link(h):
    return (h["base_bytes"] + h["patch_payload_bytes"]) / 32.0


def pick(hyb, traj):
    """The LOW-RES base at the sparing threshold: the actual hybrid proposition. The
    1088-per-eye arm is not a hybrid, it is "send HEVC instead", and it is discussed in
    the ADR rather than drawn here as though it were the same scheme."""
    return next(h for h in hyb if h["traj"] == traj and h["eye_w"] == 544
                and h["mbit"] == 10 and h["tau"] == "34dB")


def curve(base, t):
    return sorted((r["psnr"], r["bytes_per_frame"], r) for r in base if r["traj"] == t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", dest="out", required=True)
    a = ap.parse_args()
    R = json.load(open(a.inp))
    base, hyb = R["baseline"], R["hybrid"]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13.0, 7.2), facecolor=SURFACE)
    x = np.arange(len(TRAJ))
    w = 0.36

    today, hb, hp, reach, chosen = [], [], [], [], []
    for t in TRAJ:
        c = curve(base, t)
        h = pick(hyb, t)
        chosen.append(h)
        ps = [p for p, _, _ in c]
        bs = [b for _, b, _ in c]
        if h["psnr"] <= ps[-1]:
            today.append(float(np.exp(np.interp(h["psnr"], ps, np.log(bs)))))
            reach.append(True)
        else:
            # nxvc-only never reaches this quality on this trajectory at any quantiser
            # in the sweep: show its best point and say so, rather than drop the bar.
            today.append(bs[-1])
            reach.append(False)
        hb.append(h["base_bytes"] / 32.0)
        hp.append(h["patch_payload_bytes"] / 32.0)

    b1 = ax1.bar(x - w / 2, today, w, color=C_TODAY, edgecolor=SURFACE, linewidth=1.2,
                 label="nxvc only, at the hybrid's PSNR", zorder=3)
    for i, ok in enumerate(reach):
        if not ok:
            b1[i].set_hatch("///")
            b1[i].set_edgecolor("#ffffff")
    ax1.bar(x + w / 2, hb, w, color=C_BASE, edgecolor=SURFACE, linewidth=1.2,
            label="hybrid: the HEVC base layer", zorder=3)
    ax1.bar(x + w / 2, hp, w, bottom=hb, color=C_HYB, edgecolor=SURFACE, linewidth=1.2,
            label="hybrid: the nxvc patches", zorder=3)
    top = max(max(today), max(b + p for b, p in zip(hb, hp)))
    for i, (a_, b_, p_, ok) in enumerate(zip(today, hb, hp, reach)):
        ax1.annotate(f"{(b_ + p_) / a_:.1f}x" + ("" if ok else "*"),
                     (i + w / 2, b_ + p_), textcoords="offset points", xytext=(0, 4),
                     ha="center", fontsize=9, color=INK2)
    ax1.set_ylim(0, top * 1.18)
    ax1.set_ylabel("bytes per frame, both eyes")
    ax1.set_title("What it costs the link", fontsize=11.5, color=INK)
    ax1.legend(frameon=False, fontsize=8.5, loc="upper center",
               bbox_to_anchor=(0.5, -0.10), ncol=1)
    note = ("* hatched: nxvc-only never reaches the hybrid's PSNR at any quantiser in the "
            "sweep, so its best point is shown and the ratio flatters the hybrid."
            if not all(reach) else "")

    tc, tw, tf, hc, shares = [], [], [], [], []
    for t, h in zip(TRAJ, chosen):
        rows = [r for r in base if r["traj"] == t]
        ref = min(rows, key=lambda r: abs(r["psnr"] - h["psnr"]))
        sh = ref["picture_share"]
        shares.append(sh)
        tc.append(ref["coded_per_frame"])
        # A skipped tile is warped at 34 us only in a PICTURE frame. In an ATLAS frame it
        # writes no reference pixels and costs a matrix compose, which the bench puts at
        # 0.0048 ms for the whole picture -- nothing. Drawing all skips as one bar would
        # say the atlas never happened.
        tw.append(ref["skip_per_frame"] * sh)
        tf.append(ref["skip_per_frame"] * (1 - sh))
        hc.append(h["coded_per_frame"])
    hbse = [TILES - c for c in hc]

    ax2.bar(x - w / 2, tc, w, color=C_TODAY, edgecolor=SURFACE, linewidth=1.2,
            label="today: nxvc coded tiles (~41 us)", zorder=3)
    ax2.bar(x - w / 2, tw, w, bottom=tc, color=C_TODAY_L, edgecolor=SURFACE,
            linewidth=1.2, label="today: WARP_SKIP warped in a PICTURE frame (34 us)",
            zorder=3)
    ax2.bar(x - w / 2, tf, w, bottom=[a + b for a, b in zip(tc, tw)], color="#e8eef6",
            edgecolor=SURFACE, linewidth=1.2,
            label="today: WARP_SKIP resident in the atlas (a matrix compose, ~0)", zorder=3)
    ax2.bar(x + w / 2, hc, w, color=C_HYB, edgecolor=SURFACE, linewidth=1.2,
            label="hybrid: nxvc coded tiles (~41 us)", zorder=3)
    ax2.bar(x + w / 2, hbse, w, bottom=hc, color=C_BASE, edgecolor=SURFACE,
            linewidth=1.2, label="hybrid: base-sourced atlas patches (1.9 us)", zorder=3)

    ms_t = [min((r for r in base if r["traj"] == t),
                key=lambda r: abs(r["psnr"] - h["psnr"]))["adreno_ms"]
            for t, h in zip(TRAJ, chosen)]
    ms_h = [h["adreno_ms"] for h in chosen]
    for i, (mt, mh, sh) in enumerate(zip(ms_t, ms_h, shares)):
        ax2.annotate(f"{mt:.1f} $\\rightarrow$ {mh:.1f} ms", (i, TILES),
                     textcoords="offset points", xytext=(0, 20), ha="center",
                     fontsize=8.5, color=INK)
        ax2.annotate(f"{sh*100:.0f} % PICTURE", (i, TILES), textcoords="offset points",
                     xytext=(0, 5), ha="center", fontsize=7.5, color=INK2)
    ax2.set_ylim(0, TILES * 1.18)
    ax2.set_ylabel("tiles per frame the Adreno touches, both eyes of 578")
    ax2.set_title("What it saves the headset's GPU", fontsize=11.5, color=INK)
    ax2.legend(frameon=False, fontsize=8.5, loc="upper center",
               bbox_to_anchor=(0.5, -0.10), ncol=1)

    for ax in (ax1, ax2):
        ax.set_facecolor(SURFACE)
        ax.set_xticks(x)
        ax.set_xticklabels([LABEL[t] for t in TRAJ], fontsize=9)
        ax.grid(True, axis="y", color=GRID, linewidth=0.8, zorder=0)
        ax.set_axisbelow(True)
        for sp in ("top", "right"):
            ax.spines[sp].set_visible(False)
        for sp in ("left", "bottom"):
            ax.spines[sp].set_color(GRID)
        ax.tick_params(colors=INK2, labelsize=9)
        ax.yaxis.label.set_color(INK2)

    fig.suptitle("The 544-per-eye HEVC base layer, priced on headset GPU time instead of bytes",
                 fontsize=13.5, color=INK, y=0.985)
    fig.tight_layout(rect=(0, 0.035 if note else 0, 1, 0.955))
    if note:
        fig.text(0.012, 0.012, note, fontsize=8, color=INK2)
    fig.savefig(a.out + "/hybrid-gpu-time.png", dpi=110, facecolor=SURFACE)
    print("wrote", a.out + "/hybrid-gpu-time.png")


if __name__ == "__main__":
    main()
