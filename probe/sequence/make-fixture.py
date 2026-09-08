#!/usr/bin/env python3
"""Stream a deterministic moving-square stereo YUV fixture through a FIFO."""
import argparse, errno, hashlib, json, os, subprocess, tempfile, time
from pathlib import Path

def frames(width, height, count):
    assert width == 4352 and height == 2176, "fixture geometry is intentionally fixed"
    eye_w, square = width // 2, 256
    c_w, c_h = width // 2, height // 2
    chroma = bytes([128]) * (c_w * c_h)
    ybase = bytearray([32]) * width
    xr, yr = eye_w - square, height - square
    for n in range(count):
        x0 = (17 * n) % (xr + 1)
        y0 = (11 * n) % (yr + 1)
        # Opposite horizontal phase keeps both eyes temporally nonconstant.
        x1 = xr - x0
        rows = []
        for y in range(height):
            row = bytearray(ybase)
            if y0 <= y < y0 + square:
                row[x0:x0 + square] = bytes([220]) * square
                row[eye_w + x1:eye_w + x1 + square] = bytes([220]) * square
            rows.append(row)
        for row in rows:
            yield row
        yield from (chroma, chroma)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--encoder", required=True, help="path to nxvc-vkenc")
    ap.add_argument("--frames", type=int, default=1200)
    ap.add_argument("--out", type=Path, default=Path("sparse-motion-4352x2176.nxv"))
    ap.add_argument("--log", type=Path)
    args = ap.parse_args()
    if args.frames <= 0: ap.error("--frames must be positive")
    fifo = Path(tempfile.gettempdir()) / f"nxvc-sparse-motion-{os.getpid()}.yuv"
    os.mkfifo(fifo)
    log = args.log or args.out.with_suffix(".encode.log")
    cmd = [args.encoder, "--in", str(fifo), "--w", "4352", "--h", "2176",
           "--pix", "yuv420p", "--frames", str(args.frames), "--eyes", "2",
           "--qp", "40", "--atlas", "--atlas-mode", "--inter", "--entropy", "lite",
           "--out", str(args.out)]
    proc = None
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
        "fixture": "sparse-motion",
        "frames": args.frames,
        "width": 4352, "height": 2176, "eyes": 2, "pixel_format": "yuv420p",
        "square": 256, "background_y": 32, "square_y": 220,
        "source_transport": "FIFO (raw YUV never written to disk)",
        "command": cmd, "nxv_sha256": hashlib.sha256(data).hexdigest(),
        "encode_log": str(log),
    }
    args.out.with_suffix(args.out.suffix + ".json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))

if __name__ == "__main__": main()
