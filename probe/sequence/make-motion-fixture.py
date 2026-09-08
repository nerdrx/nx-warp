#!/usr/bin/env python3
"""Stream deterministic native-size stereo YUV fixtures through a FIFO."""
import argparse, errno, hashlib, json, math, os, subprocess, tempfile, time
from pathlib import Path

def dense_scene(width, height):
    """Build an edge-rich, dense static stereo scene once, then stream it."""
    eye_w = width // 2
    y_rows = []
    for y in range(height):
        row = bytearray(width)
        for eye in range(2):
            base = eye * eye_w
            for x in range(eye_w):
                # Fine texture plus large hard edges exercises warp residuals.
                block = ((x // 64) ^ (y // 64) ^ eye) & 1
                texture = (19 * x + 23 * y + 37 * ((x ^ y) & 31)) & 31
                value = 42 + texture + (48 if block else 0)
                if (x // 256 + y // 192 + eye) % 5 == 0:
                    value = 210 - ((x + y) & 15)
                row[base + x] = value
        y_rows.append(bytes(row))
    c_w, c_h = width // 2, height // 2
    chroma = bytearray(c_w * c_h)
    for y in range(c_h):
        for x in range(c_w):
            chroma[y * c_w + x] = 96 + ((11 * x + 7 * y) & 63)
    return y_rows, bytes(chroma)


def frames(width, height, count):
    assert width == 4352 and height == 2176, "fixture geometry is intentionally fixed"
    y_rows, chroma = dense_scene(width, height)
    for n in range(count):
        for row in y_rows:
            yield row
        yield from (chroma, chroma)


def write_poses(path, count):
    # Static -> moderate yaw -> large yaw -> static recovery.  The quaternion
    # convention is the one consumed by nxvc-vkenc (nxv-openxr-1).
    angles = []
    for n in range(count):
        if n < count // 5:
            deg = 0.0
        elif n < 2 * count // 5:
            deg = 18.0 * (n - count // 5) / max(1, count // 5 - 1)
        elif n < 3 * count // 5:
            deg = 18.0 + 62.0 * (n - 2 * count // 5) / max(1, count // 5 - 1)
        elif n < 4 * count // 5:
            deg = 80.0 * (4 * count // 5 - 1 - n) / max(1, count // 5 - 1)
        else:
            deg = 0.0
        half = deg * 3.141592653589793 / 360.0
        angles.append({"orientation_xyzw": [0.0, math.sin(half), 0.0, math.cos(half)]})
    path.write_text(json.dumps({"id": "nxv-openxr-1", "fov_deg": {"h": 95, "v": 95},
                                "frames": angles}, separators=(",", ":")) + "\n")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--encoder", required=True, help="path to nxvc-vkenc")
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--qp", type=int, default=40)
    ap.add_argument("--out", type=Path, default=Path("dense-motion-4352x2176.nxv"))
    ap.add_argument("--log", type=Path)
    ap.add_argument("--pose-out", type=Path)
    ap.add_argument("--atlas-picture-d", type=int, default=8)
    args = ap.parse_args()
    if not 0 <= args.qp <= 63: ap.error("--qp must be in 0..63")
    if args.frames < 5: ap.error("--frames must be at least 5")
    if args.atlas_picture_d < 0: ap.error("--atlas-picture-d must be nonnegative")
    for path in (args.out, args.log, args.pose_out):
        if path is not None:
            path.parent.mkdir(parents=True, exist_ok=True)
    fifo = Path(tempfile.gettempdir()) / f"nxvc-dense-motion-{os.getpid()}.yuv"
    log = args.log or args.out.with_suffix(".encode.log")
    pose_path = args.pose_out or args.out.with_suffix(".poses.json")
    write_poses(pose_path, args.frames)
    cmd = [args.encoder, "--in", str(fifo), "--w", "4352", "--h", "2176",
           "--pix", "yuv420p", "--frames", str(args.frames), "--eyes", "2",
           "--qp", str(args.qp), "--atlas", "--atlas-mode", "--inter", "--entropy", "lite",
           "--atlas-picture-d", str(args.atlas_picture_d), "--poses", str(pose_path),
           "--out", str(args.out)]
    proc = None
    os.mkfifo(fifo)
    try:
        with log.open("w") as lf:
            proc = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT)
            # Do not block forever if the encoder exits before opening its input.
            while True:
                if proc.poll() is not None:
                    raise RuntimeError(f"encoder exited before opening FIFO; see {log}")
                try:
                    pipe_fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                    break
                except OSError as error:
                    if error.errno != errno.ENXIO:
                        raise
                    time.sleep(0.02)
            os.set_blocking(pipe_fd, True)
            with os.fdopen(pipe_fd, "wb") as pipe:
                for chunk in frames(4352, 2176, args.frames):
                    pipe.write(chunk)
        rc = proc.wait()
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        fifo.unlink(missing_ok=True)
    if rc:
        raise SystemExit(f"encoder failed with exit status {rc}; see {log}")
    data = args.out.read_bytes()
    metadata = {
        "fixture": "dense-motion",
        "scope": "Static pixels with changing pose metadata: adversarial residual stress, not a physically rendered head turn",
        "frames": args.frames,
        "width": 4352, "height": 2176, "eyes": 2, "pixel_format": "yuv420p",
        "pose_json": str(pose_path), "pose_sha256": hashlib.sha256(pose_path.read_bytes()).hexdigest(),
        "atlas_picture_d": args.atlas_picture_d, "qp": args.qp,
        "source_transport": "FIFO (raw YUV never written to disk)",
        "command": cmd, "nxv_sha256": hashlib.sha256(data).hexdigest(),
        "encode_log": str(log),
    }
    args.out.with_suffix(args.out.suffix + ".json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))

if __name__ == "__main__": main()
