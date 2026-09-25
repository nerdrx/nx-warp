#!/usr/bin/env python3
"""Run paired standalone Vulkan binaries in ABBA order after the candidate is frozen."""
from __future__ import annotations

import argparse
import csv
import os
import subprocess
from pathlib import Path


CASES = {
    "forest-pan": ("forest-shift0.native.rgba", "forest-shift8.native.rgba", False),
    "dark-pan": ("dark-shift0.native.rgba", "dark-shift8.native.rgba", False),
    "scene-cut": ("forest-shift0.native.rgba", "dark-shift0.native.rgba", False),
    "independent-noack": ("forest-shift0.native.rgba", "forest-shift8.native.rgba", True),
    "dark-noack": ("dark-shift0.native.rgba", "dark-shift8.native.rgba", True),
}


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--fixture-dir", type=Path, required=True,
                   help="directory of native buffers produced by extract_native.py")
    p.add_argument("--output", type=Path, required=True, help="CSV destination")
    p.add_argument("--regions", choices=("global", "regional", "both"), default="both")
    p.add_argument("--cases", nargs="+", choices=tuple(CASES), default=list(CASES))
    p.add_argument("--baseline", type=Path, default=Path(__file__).parent / "motion-gpu-baseline")
    p.add_argument("--candidate", type=Path, default=Path(__file__).parent / "motion-gpu-candidate")
    args = p.parse_args()
    fixture_dir = args.fixture_dir.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["case", "regions", "run", "variant", "phase", "frame", "mode", "body_version",
                         "wire_bytes", "detail_bytes", "encode_ms", "raw_fnv64", "oracle_match"])
        for case in args.cases:
            base_name, next_name, noack = CASES[case]
            for regions in ((False, True) if args.regions == "both" else (args.regions == "regional",)):
                for run_no, variant in enumerate(("baseline", "candidate", "candidate", "baseline")):
                    binary = args.baseline if variant == "baseline" else args.candidate
                    env = os.environ.copy()
                    env.update({
                        "NX_DIRECT_CASE": case,
                        "NX_DIRECT_FIXTURE_BASE": str(fixture_dir / base_name),
                        "NX_DIRECT_FIXTURE_NEXT": str(fixture_dir / next_name),
                        "NX_DIRECT_MOTION_REGIONS": "1" if regions else "0",
                        "NX_DIRECT_ROW_PREDICTOR": "1" if variant == "candidate" else "0",
                        "NX_DIRECT_TEST_NO_ACK": "1" if noack else "0",
                    })
                    result = subprocess.run([str(binary)], env=env, text=True, capture_output=True, check=True)
                    sample = 0
                    for line in result.stdout.splitlines():
                        if not line.startswith("FRAME,"):
                            continue
                        _, row_case, frame, mode, body_version, wire_bytes, detail_bytes, elapsed, raw_hash, oracle_match = line.split(",")
                        phase = "control" if frame != "measure" else "measure"
                        if phase == "measure":
                            sample += 1
                        phase_label = "control" if phase == "control" else f"measure-{sample:02d}"
                        writer.writerow([row_case, "regional" if regions else "global", run_no, variant,
                                         phase_label, frame, mode, body_version, wire_bytes, detail_bytes,
                                         elapsed, raw_hash, oracle_match])
                    f.flush()
                    if sample != 24:
                        raise RuntimeError(f"{case}/{variant}/regions={regions}: expected 24 timed samples, got {sample}")
                    if result.stderr:
                        print(result.stderr, end="")
    print(f"wrote {output}")


if __name__ == "__main__":
    main()
