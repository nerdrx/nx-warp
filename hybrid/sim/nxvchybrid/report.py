"""Render a sweep JSON into the markdown tables of hybrid/RESULTS.md."""

from __future__ import annotations

from collections import defaultdict


def _f(v, n=2, dash="--"):
    return dash if v is None else f"{v:.{n}f}"


def _table(rows, header, align=None) -> str:
    align = align or ["---"] * len(header)
    out = ["| " + " | ".join(header) + " |", "|" + "|".join(align) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(out)


def _index(results):
    hevc, pure, hybrid = {}, {}, defaultdict(dict)
    for d in results:
        if "error" in d:
            continue
        k = d.get("kind")
        if k == "hevc":
            hevc[d["total_mbit"]] = d
        elif k == "pure":
            pure[d["total_mbit"]] = d
        elif k == "hybrid":
            hybrid[(d["base_scale"], d["total_mbit"])][d["base_frac"]] = d
    return hevc, pure, hybrid


def render(sweep: dict, extra=None) -> str:
    results = sweep["results"]
    hevc, pure, hybrid = _index(results)
    totals = sorted(hevc)
    scales = sorted({k[0] for k in hybrid}, reverse=True)
    fracs = sorted({f for v in hybrid.values() for f in v})

    L: list[str] = []
    A = L.append

    A("## Anchors")
    A("")
    A("`hevc` is x265 alone at the full bitrate and full resolution, same")
    A("zerolatency P-only settings as the base layer. `pure` is the")
    A("intra/inter DCT model of our codec with no base layer: the same")
    A("transform, quantiser, bit model, tile grid, pose warp and per-tile MV")
    A("as the enhancement layer, so hybrid-vs-pure is an internally")
    A("consistent comparison.")
    A("")
    rows = []
    for t in totals:
        h = hevc.get(t, {})
        p = pure.get(t, {})
        rows.append([
            f"{t:g}", _f(h.get("measured_mbit"), 1), _f(h.get("psnr_y")),
            _f(h.get("ssim_y"), 4), _f(p.get("measured_mbit"), 1),
            _f(p.get("psnr_y")), _f(p.get("ssim_y"), 4),
        ])
    A(_table(rows, ["Target Mbit", "HEVC Mbit", "HEVC PSNR-Y", "HEVC SSIM",
                    "Pure Mbit", "Pure PSNR-Y", "Pure SSIM"]))
    A("")

    A("## Hybrid sweep: PSNR-Y (dB) by base resolution and base share of the bitrate")
    A("")
    for s in scales:
        A(f"### Base at {s:g}x resolution")
        A("")
        rows = []
        for t in totals:
            cells = hybrid.get((s, t), {})
            row = [f"{t:g}"]
            best = None
            for fr in fracs:
                d = cells.get(fr)
                row.append(_f(d.get("psnr_y") if d else None))
                if d and (best is None or d["psnr_y"] > best[1]["psnr_y"]):
                    best = (fr, d)
            h = hevc.get(t)
            p = pure.get(t)
            if best:
                row.append(f"**{best[1]['psnr_y']:.2f}** @ {best[0]:g}")
                row.append(_f(best[1]["psnr_y"] - h["psnr_y"]) if h else "--")
                row.append(_f(best[1]["psnr_y"] - p["psnr_y"]) if p else "--")
            else:
                row += ["--", "--", "--"]
            rows.append(row)
        A(_table(rows, ["Mbit"] + [f"base {int(f*100)}%" for f in fracs]
                 + ["best", "vs HEVC", "vs pure"]))
        A("")

    A("## Hybrid sweep: SSIM (Y) at the best split of each row")
    A("")
    rows = []
    for t in totals:
        row = [f"{t:g}"]
        for s in scales:
            cells = hybrid.get((s, t), {})
            if cells:
                b = max(cells.values(), key=lambda d: d["psnr_y"])
                row.append(f"{b['ssim_y']:.4f}")
            else:
                row.append("--")
        row.append(_f(hevc[t]["ssim_y"], 4) if t in hevc else "--")
        row.append(_f(pure[t]["ssim_y"], 4) if t in pure else "--")
        rows.append(row)
    A(_table(rows, ["Mbit"] + [f"base {s:g}x" for s in scales] + ["HEVC", "pure"]))
    A("")

    A("## Which hypothesis wins")
    A("")
    A("`temporal win` is the share of enhancement tiles where the pure")
    A("pose-warped hypothesis (w=0) has lower residual energy than the pure")
    A("upsampled-base hypothesis (w=1) -- the LCEVC-style layer -- before any")
    A("blending. `mean w` is the mean signalled weight (0 = all temporal,")
    A("1 = all base). `blend gain` is the residual-energy reduction of the")
    A("chosen blend over the better of the two single hypotheses.")
    A("")
    rows = []
    for s in scales:
        for t in totals:
            cells = hybrid.get((s, t), {})
            if not cells:
                continue
            d = max(cells.values(), key=lambda x: x["psnr_y"])
            st = d["stats"]
            tw = st["temporal_wins"]
            bw = st["base_wins"]
            tot = max(1, tw + bw)
            bg = 1.0 - st["blend_sse"] / max(1e-9, st["best_single_sse"])
            wsum = sum(float(k) * v for k, v in st["weight_hist"].items())
            wn = max(1, sum(st["weight_hist"].values()))
            rows.append([
                f"{s:g}x", f"{t:g}", f"{d['base_frac']:g}",
                f"{100.0 * tw / tot:.1f}%", _f(wsum / wn, 3),
                f"{100.0 * bg:.1f}%",
                f"{100.0 * st['intra_tiles'] / max(1, st['total_tiles']):.1f}%",
            ])
    A(_table(rows, ["base res", "Mbit", "base share", "temporal win", "mean w",
                    "blend gain", "intra"]))
    A("")

    A("### Per tile class, at the recommended operating point")
    A("")
    best_overall = None
    for key, cells in hybrid.items():
        for d in cells.values():
            h = hevc.get(d["total_mbit"])
            if not h:
                continue
            gain = d["psnr_y"] - h["psnr_y"]
            if best_overall is None or gain > best_overall[0]:
                best_overall = (gain, d)
    rows = []
    if best_overall:
        d = best_overall[1]
        st = d["stats"]
        for c in ("flat", "texture", "edge", "text"):
            n = st["class_total"].get(c, 0)
            tw = st["class_temporal_wins"].get(c, 0)
            ws = st["class_weight_sum"].get(c, 0.0)
            rows.append([c, f"{n}", f"{100.0 * tw / max(1, n):.1f}%",
                         _f(ws / max(1, n), 3)])
        A(f"Operating point: base {d['base_scale']:g}x, base share "
          f"{d['base_frac']:g}, {d['total_mbit']:g} Mbit.")
        A("")
        A(_table(rows, ["tile class", "tiles", "temporal win", "mean w"]))
        A("")

    A("## Bit-model cross-check")
    A("")
    A("`model` is the `log2(1+|q|)` estimator of `codec.py`; `order-0` is the")
    A("entropy of the same quantised symbols under a context-free static")
    A("model. The ratio is the slack in the estimator: a real rANS coder with")
    A("per-band contexts lands between the two.")
    A("")
    rows = []
    for s in scales:
        for t in totals:
            cells = hybrid.get((s, t), {})
            if not cells:
                continue
            d = max(cells.values(), key=lambda x: x["psnr_y"])
            st = d["stats"]
            m = st["model_bits"]
            e = st["empirical_bits"]
            rows.append([f"{s:g}x", f"{t:g}", f"{m/1e6:.2f}", f"{e/1e6:.2f}",
                         _f(e / max(1.0, m), 3)])
    A(_table(rows, ["base res", "Mbit", "model Mbit-total", "order-0 Mbit-total",
                    "order-0 / model"]))
    A("")

    for x in extra or []:
        A(f"## A/B: {x.get('note', 'variant')}")
        A("")
        hx, px, yx = _index(x["results"])
        rows = []
        for t in sorted(hx or px or {}):
            cells = {k: v for k, v in yx.items() if k[1] == t}
            if cells:
                d = max((d for c in cells.values() for d in c.values()),
                        key=lambda z: z["psnr_y"])
                rows.append([f"{t:g}", _f(d["psnr_y"]), _f(d["ssim_y"], 4),
                             f"{d['base_scale']:g}x", f"{d['base_frac']:g}"])
        if rows:
            A(_table(rows, ["Mbit", "PSNR-Y", "SSIM", "base res", "base share"]))
            A("")

    return "\n".join(L) + "\n"
