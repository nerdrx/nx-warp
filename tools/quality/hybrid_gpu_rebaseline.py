#!/usr/bin/env python3
"""Re-measure the nxvc-only curve under ADR-0029's RECOMMENDED policy.

The first pass ran `--atlas on` with no PICTURE-frame trigger, which ADR-0029 measures as
the losing configuration at speed (-5.9 dB at fast turn). Comparing a hybrid against that
would be comparing it against a strawman. `--atlas-picture-disp 8` is what ADR-0029
recommends and what a shipped encoder would run.
"""
import json, os
import numpy as np
import run as R

out = []
src_all = {t: R.read_y(f"{R.FIX}/{t}.yuv420p.yuv", R.W, R.H, R.FRAMES) for t in R.TRAJ}
for t in R.TRAJ:
    for qp in (22, 26, 30, 34, 38):
        p = f"{R.OUT}/{t}_d8_qp{qp}.nxv"
        b = (os.path.getsize(p) if os.path.exists(p)
             else R.nxv(t, p, qp, ["--atlas-picture-disp", "8"]))
        dec = f"{R.OUT}/{t}_d8_qp{qp}.yuv"
        R.nxv_decode(p, dec)
        y = R.read_y(dec, R.W, R.H, R.FRAMES)
        nf, coded, skipped, payload = R.tile_modes(p)
        out.append(dict(traj=t, qp=qp, bytes=b, bytes_per_frame=b / R.FRAMES,
                        psnr=R.psnr(src_all[t], y),
                        coded_per_frame=float(np.mean(coded)),
                        skip_per_frame=float(np.mean(skipped))))
        os.remove(dec)
        print("d8", t, qp, f"{out[-1]['psnr']:.2f}", f"{out[-1]['bytes_per_frame']:.0f}",
              f"coded {out[-1]['coded_per_frame']:.0f}", flush=True)

full = json.load(open(f"{R.OUT}/results.json"))
full["baseline_atlas_only"] = full["baseline"]
full["baseline"] = out
json.dump(full, open(f"{R.OUT}/results.json", "w"), indent=1)
print("done")
