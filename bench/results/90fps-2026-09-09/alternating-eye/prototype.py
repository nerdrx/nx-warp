"""CPU-only experiment: synchronized versus alternating stereo detail refresh.

This is an image-quality model, not a codec, GPU benchmark, or latency test.
The only high-resolution source reads made by a run are the scheduled refresh
mask and the native centre; the current peripheral source is used only to
score the result after rendering.
"""
from pathlib import Path
import json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

OUT = Path(__file__).resolve().parent
H = W = 512
FRAMES = 49  # 48 steady-state frames: balanced two-frame schedule pairs.
TILE = 16
MAX_AGE = 8
Y, X = np.mgrid[:H, :W]
CENTRE = (Y >= 192) & (Y < 320) & (X >= 192) & (X < 320)
PERIPH = np.array([i for i in range(256)
                   if not (6 <= i // 16 < 10 and 6 <= i % 16 < 10)])
STEPS = np.array([0] + [1 if f % 2 else 3 for f in range(1, FRAMES)])


def shift(a, dx, fill=0):
    out = np.full_like(a, fill)
    valid = np.zeros(a.shape, dtype=bool)
    if 0 <= dx < W:
        out[..., dx:] = a[..., :W-dx]
        valid[..., dx:] = True
    elif -W < dx < 0:
        out[..., :W+dx] = a[..., -dx:]
        valid[..., :W+dx] = True
    return out, valid


def source(frame):
    images, masks = [], []
    pan = int(STEPS[:frame + 1].sum())
    for eye in range(2):
        wx = X - pan + eye * 2
        bg = .42 + .12*np.sin(wx/13) + .10*np.sin(Y/19)
        bg += .08*np.sin((wx + Y)/7) + .13*((wx % 11) < 2)
        bg -= .09*((Y % 17) < 2)
        x0, y0 = 55 + 5*frame + eye*8, 280 + int(22*np.sin(frame/6))
        fg = (X >= x0) & (X < x0+104) & (Y >= y0) & (Y < y0+92)
        qx, qy = 375 - 3*frame + eye*5, 90 + int(16*np.cos(frame/5))
        q = (X >= qx) & (X < qx+45) & (Y >= qy) & (Y < qy+58)
        img = np.where(fg, .82 + .12*((X-x0) % 7 < 2), bg)
        img = np.where(q, .15 + .2*((Y-qy) % 9 < 3), img)
        images.append(np.rint(np.clip(img, 0, 1)*255).astype(np.float32)/255)
        masks.append(fg | q)
    return np.array(images), np.array(masks)


def guide(a):
    return a.reshape(2, H//2, 2, W//2, 2).mean((2, 4))


def up(a):
    return a.repeat(2, 1).repeat(2, 2)


def estimate_motion(hist, current_guide, details=False):
    """Small horizontal search using retained history and the current guide."""
    candidates = []
    for dx in range(-4, 5):
        candidate, valid = shift(hist, dx)
        e = np.abs(current_guide - guide(candidate))
        candidates.append((float(np.median(e[:, 1:-1, 1:-1])), abs(dx), dx))
    ranked = sorted(candidates)
    result = (ranked[0][2], ranked[1][0] - ranked[0][0])
    return result if details else result[0]


def scheduled_refresh(mode, frame):
    """True means a full peripheral high-detail refresh is authorized."""
    refresh = np.zeros((2, H, W), dtype=bool)
    if mode in ("synchronized", "synchronized_even", "synchronized_odd"):
        phase = 0 if mode != "synchronized_odd" else 1
        refresh[:, ~CENTRE] = (frame % 2 == phase)
    elif mode in ("alternating_known", "alternating_estimated"):
        refresh[frame % 2, ~CENTRE] = True
    else:
        raise ValueError(mode)
    return refresh


def run(mode, drop_frame=None, reset_frame=None, max_age=MAX_AGE):
    history, previous_fg = source(0)
    age = np.zeros((2, H, W), dtype=np.int16)
    valid = np.ones((2, H, W), dtype=bool)
    records, final = [], None
    for frame in range(1, FRAMES):
        truth, fg = source(frame)  # scoring fixture; never used for unscheduled pixels
        current_guide = guide(truth)
        if mode == "alternating_estimated":
            dx, confidence_gap = estimate_motion(history, current_guide, details=True)
        else:
            dx, confidence_gap = int(STEPS[frame]), None
        warped, warp_valid = shift(history, dx)
        warped_age, _ = shift(age, dx, MAX_AGE + 1)
        warped_valid, _ = shift(valid, dx, False)
        age = warped_age + 1
        valid = warped_valid & warp_valid & (age <= max_age)
        if reset_frame == frame:
            # Decoder reset: retained high-detail history is unavailable.
            valid[:] = False
            age[:] = max_age + 1
        output = np.where(valid, warped, up(current_guide))

        # Guide mismatch and disocclusion invalidate retained high-detail pixels.
        mismatch = up(np.abs(guide(warped) - current_guide) > .11)
        old_fg, old_fg_valid = shift(previous_fg, int(STEPS[frame]), False)
        disoccluded = old_fg & ~fg & old_fg_valid & ~CENTRE[None]
        # Renderer-side rejection uses only retained history and current guide.
        # Foreground masks are scoring-only truth and never steer source reads.
        reject = (~valid) | mismatch
        if confidence_gap is not None and confidence_gap < .001:
            reject[:] = True
        output[reject] = up(current_guide)[reject]
        valid[reject] = False

        # Scheduled source reads: full peripheral detail for one or both eyes.
        refresh = scheduled_refresh(mode, frame)
        if drop_frame == frame:
            # Simulate loss of the scheduled high-detail payload. The guide is
            # still available, and the frame number keeps its source phase.
            refresh[:] = False
        output[refresh] = truth[refresh]
        age[refresh] = 0
        valid[refresh] = True
        # Native centre is always fresh for both eyes.
        output[:, CENTRE] = truth[:, CENTRE]
        age[:, CENTRE] = 0
        valid[:, CENTRE] = True
        assert np.array_equal(output[:, 192:320, 192:320], truth[:, 192:320, 192:320])

        error = np.abs(output - truth) * 255
        stereo_truth = truth[0] - truth[1]
        stereo_out = output[0] - output[1]
        records.append({
            "frame": frame,
            "mae": float(error.mean()),
            "periphery_mae": float(error[:, ~CENTRE].mean()),
            "stereo_temporal_residual_mae": float(np.abs(stereo_out-stereo_truth)[~CENTRE].mean()*255),
            "disocclusion_mae": float(error[disoccluded].mean()) if disoccluded.any() else 0.0,
            "disocclusion_pixels": int(disoccluded.sum()),
            "motion_dx": dx, "true_dx": int(STEPS[frame]),
            "motion_matches": bool(dx == STEPS[frame]),
            "confidence_gap": confidence_gap,
            "refreshed_eyes": int(refresh[:, ~CENTRE].any(axis=1).sum()),
            "refreshed_eye_pixels": int(refresh.sum()),
            "invalid_or_mismatch_fraction": float((reject & ~refresh & ~CENTRE[None]).mean()),
            "max_detail_age": int(age[valid].max()) if valid.any() else 0,
            "valid_detail_fraction": float(valid[:, ~CENTRE].mean()),
            "valid_detail_fraction_by_eye": [float(valid[e, ~CENTRE].mean()) for e in range(2)],
        })
        history, previous_fg = output, fg
        final = {"truth": truth, "output": output.copy(), "valid": valid.copy(), "age": age.copy()}
    return records, final


def loss_reset_stress():
    rows, _ = run("alternating_known", drop_frame=2, reset_frame=5, max_age=2)
    return {
        "mode": "alternating_known",
        "dropped_source_frame": 2,
        "reset_source_frame": 5,
        "max_age_bound": 2,
        "max_observed_detail_age": max(r["max_detail_age"] for r in rows),
        "guide_fallback_after_drop": rows[1]["invalid_or_mismatch_fraction"],
        "guide_fallback_at_reset": rows[4]["invalid_or_mismatch_fraction"],
        "reset_unrefreshed_eye_valid_detail_fraction": rows[4]["valid_detail_fraction_by_eye"][0],
        "motion_phase_uses_source_frame": True,
        "bounded": all(r["max_detail_age"] <= 2 for r in rows),
    }


def main():
    traces, finals, summaries = {}, {}, {}
    for mode in ("synchronized_even", "synchronized_odd", "alternating_known", "alternating_estimated"):
        traces[mode], finals[mode] = run(mode)
        rows = traces[mode]
        total_refresh = sum(r["refreshed_eye_pixels"] for r in rows)
        total_disocclusion = sum(r["disocclusion_pixels"] for r in rows)
        summaries[mode] = {
            "mae": float(np.mean([r["mae"] for r in rows])),
            "periphery_mae": float(np.mean([r["periphery_mae"] for r in rows])),
            "stereo_temporal_residual_mae": float(np.mean([r["stereo_temporal_residual_mae"] for r in rows])),
            "disocclusion_mae": (sum(r["disocclusion_mae"]*r["disocclusion_pixels"] for r in rows)/total_disocclusion
                                 if total_disocclusion else 0.0),
            "motion_matches_fraction": float(np.mean([r["motion_matches"] for r in rows])),
            "mean_invalid_or_mismatch_fraction": float(np.mean([r["invalid_or_mismatch_fraction"] for r in rows])),
            "max_detail_age": max(r["max_detail_age"] for r in rows),
            "refresh_eye_pixels": total_refresh,
            "refresh_eye_pixel_budget_ratio": total_refresh / (len(rows)*2*int((~CENTRE).sum())),
        }

    for sync in ("synchronized_even", "synchronized_odd"):
        assert sum(r["refreshed_eye_pixels"] for r in traces[sync]) == sum(r["refreshed_eye_pixels"] for r in traces["alternating_known"])

    metrics = {
        "width_per_eye": W, "height": H, "frames": FRAMES,
        "steady_state_frames": FRAMES-1,
        "schedule": "synchronized_even/odd: both eyes full peripheral refresh on the named phase; alternating: eye (frame mod 2) refreshes every frame",
        "fair_budget": "Both modes refresh exactly one full peripheral eye per frame on average (same eye-pixel reads over each two-frame pair); both native centres refresh every frame.",
        "fused_packed_grid_sample_proxy": {
            "current_packed_output_samples": 861184,
            "cheap_guide_plus_centre_samples": 411904,
            "two_frame_average_samples": 636544,
            "saving_ceiling_percent": 26.1,
            "meaning": "Separate geometry proxy supplied for the live packed grid; full-detail eye needs no redundant guide transmission. Excludes history reads, compute, codec overhead, and GPU work."
        },
        "guide": "Current half-width and half-height box-averaged grayscale guide for both eyes every frame.",
        "history_guard": "Retained detail is motion-warped, then rejected on invalid warp, age > 8, guide mismatch > 0.11. Foreground masks are scoring-only; disocclusions may escape guide rejection.",
        "motion": "known uses renderer translation; estimated searches horizontal -4..4 from guide/history only; foreground motion is not supplied.",
        "estimated_confidence_guard": "When best-vs-runner-up median guide error gap is <0.001, all retained detail is rejected and the guide is used; this is a heuristic guard.",
        "stereo_metric_limits": "stereo_temporal_residual_mae is intensity residual of (left-right) versus truth, a disparity/residual proxy rather than geometric disparity; no depth, rotation, codec, transport, or GPU cost is modeled.",
        "source_read_limits": "truth forms the current guide and supplies scheduled detail/centre pixels; no other fresh detail or foreground masks steer reconstruction. Full truth is also used for offline scoring.",
        "summaries": summaries, "traces": traces,
        "loss_reset_stress": loss_reset_stress(),
    }
    (OUT / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    np.savez_compressed(OUT / "final_review.npz", **{m: finals[m]["output"] for m in finals},
                        truth=finals["alternating_known"]["truth"], valid=finals["alternating_known"]["valid"])
    # Last frame is even: right eye is the cheap eye, so show its retained output.
    eye = 1
    truth = finals["alternating_known"]["truth"]
    half = up(guide(truth)); half[:, CENTRE] = truth[:, CENTRE]
    alt = finals["alternating_known"]["output"][eye]
    fig, axes = plt.subplots(2, 3, figsize=(12, 8))
    panels = [(truth[eye], "Source (right eye)"),
              (half[eye], "Half-res guide + fresh centre"),
              (alt, "Alternating: cheap right eye"),
              (finals["synchronized_even"]["output"][eye], "Synchronized: detail due"),
              (finals["synchronized_odd"]["output"][eye], "Synchronized: guide due")]
    for ax, (img, label) in zip(axes.flat, panels):
        ax.imshow(img, cmap="gray", vmin=0, vmax=1); ax.set_title(label, fontsize=10); ax.axis("off")
    error = np.abs(alt-truth[eye])*255
    im = axes[1, 2].imshow(error, cmap="magma", vmin=0, vmax=32)
    axes[1, 2].set_title("Alternating absolute error (clipped at 32)", fontsize=9); axes[1, 2].axis("off")
    fig.colorbar(im, ax=axes[1, 2], shrink=.75, label="Code values")
    fig.suptitle("Alternating detail, frame 48 — synthetic CPU fixture, not Pico")
    fig.tight_layout(); fig.savefig(OUT / "comparison.png", dpi=140); plt.close(fig)
    fig, axes = plt.subplots(1, 2, figsize=(11, 4))
    for mode, rows in traces.items():
        axes[0].plot([r["frame"] for r in rows], [r["periphery_mae"] for r in rows], label=mode)
        axes[1].plot([r["frame"] for r in rows], [r["stereo_temporal_residual_mae"] for r in rows], label=mode)
    axes[0].set(title="Peripheral error", ylabel="MAE (0–255)")
    axes[1].set(title="Stereo temporal residual", ylabel="MAE (0–255)")
    for ax in axes: ax.set_xlabel("Frame"); ax.grid(alpha=.2); ax.legend(fontsize=8)
    fig.tight_layout(); fig.savefig(OUT / "trace.png", dpi=140); plt.close(fig)
    print(json.dumps(summaries, indent=2))


if __name__ == "__main__":
    main()
