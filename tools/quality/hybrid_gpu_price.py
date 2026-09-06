#!/usr/bin/env python3
"""Re-price the hybrid HEVC base layer on headset GPU time.

Everything shells out to the real tools -- x265 through ffmpeg, nxv-enc/nxv-dec/nxv-info
from the installed nxvc -- so every number here is a measurement of a real bitstream.
"""
import json, os, subprocess, sys, shutil
import numpy as np

FIX = "/run/media/nerdrx/Lex/claude/nx-scratch/fixtures/vrroom"
OUT = "/run/media/nerdrx/Lex/claude/nx-scratch/hybgpu"
NICE = ["chrt", "-i", "0", "taskset", "-c", "12-15", "nice", "-n", "19"]
W, H = 2176, 1088          # side-by-side; one eye is 1088x1088
TILE = 64
TCOLS, TROWS = W // TILE, H // TILE   # 34 x 17 = 578 over the pair
NTILES = TCOLS * TROWS
FRAMES = 32
FPS = 90.0
TRAJ = ["still", "rest", "mid", "fast", "objmotion", "objmotion-still"]

def sh(cmd, **kw):
    return subprocess.run(NICE + cmd, check=True, capture_output=True, text=True, **kw)

def frame_bytes(w, h):
    return w * h * 3 // 2

def read_y(path, w, h, n):
    """Luma planes only, as uint8 [n, h, w]."""
    fb = frame_bytes(w, h)
    out = np.empty((n, h, w), np.uint8)
    with open(path, "rb") as f:
        for i in range(n):
            f.seek(i * fb)
            out[i] = np.frombuffer(f.read(w * h), np.uint8).reshape(h, w)
    return out

def read_yuv(path, w, h, n):
    fb = frame_bytes(w, h)
    buf = np.fromfile(path, np.uint8, count=fb * n)
    return buf.reshape(n, fb)

def psnr(a, b):
    d = a.astype(np.int32) - b.astype(np.int32)
    mse = float(np.mean(d * d))
    return 99.0 if mse == 0 else 10.0 * np.log10(255.0 * 255.0 / mse)

# ---------------------------------------------------------------- the base layer
def base_encode(traj, eye_w, mbit):
    """libx265 at `eye_w` per eye and `mbit` Mbit/s, then decode and upsample back.

    Returns (bytes, path to the upsampled full-size yuv). The rate is held with a VBV,
    because without one x265 undershoots a 0.36 s clip by half and the label would be a
    lie."""
    sbs_w, sbs_h = eye_w * 2, eye_w
    tag = f"{traj}_b{eye_w}_{mbit}M"
    hevc = f"{OUT}/{tag}.hevc"
    dec = f"{OUT}/{tag}.dec.yuv"
    up = f"{OUT}/{tag}.up.yuv"
    src = f"{FIX}/{traj}.yuv420p.yuv"
    if not os.path.exists(hevc):
        # LOW LATENCY, or the number is a lie. A base layer feeding a 90 Hz headset
        # cannot reorder frames or look ahead: bframes=0 and rc-lookahead=0 are not a
        # tuning choice, they are the only configuration that can be shipped. x265's
        # defaults (B-frames, a 20-frame lookahead) make the base look several dB
        # better than anything a real session could produce, which is exactly how this
        # comparison goes wrong in the hybrid's favour.
        params = (f"keyint=32:min-keyint=32:scenecut=0:bframes=0:b-adapt=0:"
                  f"rc-lookahead=0:lookahead-slices=0:frame-threads=1:"
                  f"log-level=error:pools=4:"
                  f"vbv-maxrate={mbit*1000}:vbv-bufsize={mbit*1000//3}:strict-cbr=1")
        sh(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-f", "rawvideo",
            "-pix_fmt", "yuv420p", "-s", f"{W}x{H}", "-r", str(FPS), "-i", src,
            "-vf", f"scale={sbs_w}:{sbs_h}:flags=lanczos", "-c:v", "libx265",
            "-tune", "zerolatency", "-b:v", f"{mbit}M", "-x265-params", params,
            "-f", "hevc", hevc])
    if not os.path.exists(up):
        sh(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", hevc,
            "-pix_fmt", "yuv420p", "-f", "rawvideo", dec])
        # The headset upsamples the base in its display pass; bilinear is what a
        # sampler does, so bilinear is what is priced here -- not lanczos, which the
        # display pass will not be running.
        sh(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-f", "rawvideo",
            "-pix_fmt", "yuv420p", "-s", f"{sbs_w}x{sbs_h}", "-i", dec,
            "-vf", f"scale={W}:{H}:flags=bilinear", "-pix_fmt", "yuv420p",
            "-f", "rawvideo", up])
        os.remove(dec)
    return os.path.getsize(hevc), up

# ---------------------------------------------------------------- the patch set
def tile_mse(src_y, base_y):
    """Per-frame, per-tile luma MSE between the source and the upsampled base."""
    n = src_y.shape[0]
    d = src_y.astype(np.int32) - base_y.astype(np.int32)
    d = (d * d).reshape(n, TROWS, TILE, TCOLS, TILE)
    return d.mean(axis=(2, 4))          # [n, TROWS, TCOLS]

def write_skip_map(path, patch):
    """nxv-enc --skip-map: tile_count bytes per frame, 1 = force WARP_SKIP.
    A tile we do NOT patch is forced to skip, so the coded tiles are exactly the
    patch set."""
    with open(path, "wb") as f:
        for fr in patch:
            f.write((~fr.reshape(-1).astype(bool)).astype(np.uint8).tobytes())

# ---------------------------------------------------------------- nxvc
def nxv(traj, out, qp, extra=()):
    src = f"{FIX}/{traj}.yuv420p.yuv"
    cmd = ["nxv-enc", "--in", src, "--w", str(W), "--h", str(H), "--pix", "yuv420p",
           "--eyes", "2", "--poses", f"{FIX}/{traj}.poses.json", "--inter", "on",
           "--atlas", "on", "--row-present", "on", "--qp", str(qp),
           "--frames", str(FRAMES), "--threads", "4", "--out", out, "--quiet"]
    sh(cmd + list(extra))
    return os.path.getsize(out)

def nxv_decode(nxv_path, out_yuv):
    sh(["nxv-dec", "--in", nxv_path, "--out", out_yuv, "--pix", "yuv420p", "--quiet"])

def tile_modes(nxv_path):
    """Per-frame tile mode histogram from nxv-info --tiles.
    Returns (frames, coded_per_frame, skip_per_frame, payload_per_frame)."""
    r = subprocess.run(NICE + ["nxv-info", "--in", nxv_path, "--tiles"],
                       check=True, capture_output=True, text=True)
    frames, coded, skipped, payload = 0, [], [], []
    c = s = p = 0
    for line in r.stdout.splitlines():
        if line.startswith("frame "):
            if frames:
                coded.append(c); skipped.append(s); payload.append(p)
            frames += 1; c = s = p = 0
        elif line.strip().startswith("tile "):
            t = line.split()
            mode = t[3]
            if mode == "WARP_SKIP":
                s += 1
            else:
                c += 1
            # "...  52 B ..." -- the payload is the token before the "B", which is
            # not the last token on a skipped tile ("0 B mv+0,+0 ref0").
            if "B" in t:
                p += int(t[t.index("B") - 1])
    if frames:
        coded.append(c); skipped.append(s); payload.append(p)
    return frames, coded, skipped, payload

# ---------------------------------------------------------------- the experiment
def composite(base_up_y, patch_y, patch_mask):
    """The hybrid picture: the upsampled base everywhere, the nxvc patch where the
    base was not good enough. Luma only -- every number reported is PSNR-Y, which is
    what the corpus's own anchors use."""
    out = base_up_y.copy()
    n = out.shape[0]
    m = np.repeat(np.repeat(patch_mask, TILE, axis=1), TILE, axis=2)
    out[m] = patch_y[m]
    return out

def run():
    src_all = {t: read_y(f"{FIX}/{t}.yuv420p.yuv", W, H, FRAMES) for t in TRAJ}
    results = {"baseline": [], "hybrid": []}

    # The nxvc-only curve does not depend on the base layer, so a re-run of the hybrid
    # half reuses it rather than spending an hour reproducing bit-identical streams.
    prev = f"{OUT}/results-lookahead-baseline.json"
    if os.path.exists(prev):
        results["baseline"] = json.load(open(prev))["baseline"]
        print(f"reusing {len(results['baseline'])} baseline points", flush=True)

    # --- today: nxvc alone, a quantiser sweep so an equal-PSNR and an equal-bytes
    # comparison can both be read off the same curve.
    for t in ([] if results["baseline"] else TRAJ):
        for qp in (22, 26, 30, 34, 38):
            out = f"{OUT}/{t}_base_qp{qp}.nxv"
            b = nxv(t, out, qp) if not os.path.exists(out) else os.path.getsize(out)
            dec = f"{OUT}/{t}_base_qp{qp}.yuv"
            if not os.path.exists(dec):
                nxv_decode(out, dec)
            y = read_y(dec, W, H, FRAMES)
            nf, coded, skipped, payload = tile_modes(out)
            results["baseline"].append(dict(
                traj=t, qp=qp, bytes=b, bytes_per_frame=b / FRAMES,
                psnr=psnr(src_all[t], y),
                coded_per_frame=float(np.mean(coded)),
                skip_per_frame=float(np.mean(skipped))))
            os.remove(dec)
            print("baseline", t, qp, results["baseline"][-1]["psnr"], flush=True)

    # --- the hybrid: a low-res HEVC base plus nxvc patches where it is not good enough
    for t in TRAJ:
        src = src_all[t]
        for eye_w in (544, 1088):
            for mbit in (10, 15, 25):
                bb, up = base_encode(t, eye_w, mbit)
                by = read_y(up, W, H, FRAMES)
                base_psnr = psnr(src, by)
                tmse = tile_mse(src, by)
                for tau, tname in ((6.5, "40dB"), (26.0, "34dB")):
                    patch = tmse > tau
                    tag = f"{t}_h{eye_w}_{mbit}M_{tname}"
                    sm = f"{OUT}/{tag}.skip"
                    write_skip_map(sm, patch)
                    out = f"{OUT}/{tag}.nxv"
                    pb = nxv(t, out, 26, ["--skip-map", sm])
                    dec = f"{OUT}/{tag}.yuv"
                    nxv_decode(out, dec)
                    py_ = read_y(dec, W, H, FRAMES)
                    hy = composite(by, py_, patch)
                    nf, coded, skipped, payload = tile_modes(out)
                    # Patch BYTES are the tile payloads, not the file: a real hybrid
                    # sends the patch tiles and nothing else, while this stream still
                    # carries a header for every tile the row-present elision could
                    # not drop. Both are reported.
                    results["hybrid"].append(dict(
                        traj=t, eye_w=eye_w, mbit=mbit, tau=tname,
                        base_bytes=bb, base_psnr=base_psnr,
                        patch_stream_bytes=pb,
                        patch_payload_bytes=int(np.sum(payload)),
                        patch_tiles_per_frame=float(np.mean(patch.sum(axis=(1, 2)))),
                        coded_per_frame=float(np.mean(coded)),
                        skip_per_frame=float(np.mean(skipped)),
                        total_bytes=bb + pb,
                        total_bytes_payload=bb + int(np.sum(payload)),
                        psnr=psnr(src, hy)))
                    os.remove(dec); os.remove(sm)
                    print("hybrid", tag, results["hybrid"][-1]["psnr"], flush=True)
                del by
            # the upsampled bases are large; keep only what is still needed
        for f in os.listdir(OUT):
            if f.startswith(t) and f.endswith(".up.yuv"):
                os.remove(os.path.join(OUT, f))

    json.dump(results, open(f"{OUT}/results.json", "w"), indent=1)
    print("done")

if __name__ == "__main__":
    run()
