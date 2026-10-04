#!/usr/bin/env python3
"""Re-run offline ASTC quality/size checks using the packaged harness and fixtures."""
import argparse
import subprocess
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--encoder", type=Path, default=ROOT / "harness/build/astc-gpu")
    ap.add_argument("--decoder", type=Path, required=True, help="Basis Universal ASTC decode helper")
    ap.add_argument("--scenes", default="dark,forest,crowd")
    ap.add_argument("--qualities", default="6,5,4,3,2,1,0")
    ap.add_argument("--output-dir", type=Path, default=ROOT / "rerun")
    args = ap.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for scene in args.scenes.split(","):
        source = ROOT / "fixtures" / {
            "dark": "dark-left.png", "forest": "forest-left.png",
            "crowd": "crowd-500-reference.png",
        }[scene]
        image = Image.open(source).convert("RGBA")
        width, height = image.size
        raw = args.output_dir / f"{scene}.rgba"
        raw.write_bytes(image.tobytes())
        scene_dir = args.output_dir / scene
        scene_dir.mkdir(exist_ok=True)
        for q in map(int, args.qualities.split(",")):
            out = scene_dir / f"{scene}-q{q}.astc"
            subprocess.run([str(args.encoder.resolve()), str(raw.resolve()), str(out.resolve()),
                            str(width), str(height), "3", str(q), "resident"],
                           cwd=args.encoder.resolve().parent.parent, check=True)
        astc = sorted(scene_dir.glob("*.astc"))
        cmd = ["python3", str(ROOT / "compare_astc.py"), *map(str, astc),
               "--source", str(source), "--decoder", str(args.decoder.resolve()),
               "--output-dir", str(scene_dir / "metrics")]
        subprocess.run(cmd, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
