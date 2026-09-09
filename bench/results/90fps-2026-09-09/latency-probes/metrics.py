#!/usr/bin/env python3
"""Summarize PxrMetric samples inside each client's analyzed render window."""
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
STAMP = re.compile(r"^(?:\d\d-\d\d )?(\d\d:\d\d:\d\d\.\d+)")
NUM = re.compile(r"([A-Za-z]+)=([^, ]+)")
FIELDS = ("gpu_clock_mhz", "gpu_temp_c", "gpu_percent", "fps_app",
          "fps_display", "mtp_ms_runtime_estimate", "frm_gpu_ms", "atw_gpu_ms")


def window(path):
    raw = subprocess.check_output([sys.executable, str(ROOT / "analyze_live.py"), str(path)], text=True)
    return json.loads(raw.strip().splitlines()[-1])["client"]


def parse(path):
    span = window(path)
    values = {field: [] for field in FIELDS}
    for line in path.read_text(errors="replace").splitlines():
        stamp = STAMP.search(line)
        if not stamp or not (span["start"] <= stamp.group(1) <= span["end"]):
            continue
        if "PxrMetric:" not in line:
            continue
        fields = dict(NUM.findall(line.split("PxrMetric:", 1)[1]))
        try:
            app, display = fields["FPS"].split("/")
            gpu_percent, clock = fields["GPU"].split("/")
            values["fps_app"].append(float(app)); values["fps_display"].append(float(display))
            values["gpu_percent"].append(float(gpu_percent.removesuffix("%"))); values["gpu_clock_mhz"].append(float(clock.removesuffix("Mhz")))
            values["gpu_temp_c"].append(float(fields["GPUTemp"].removesuffix("C")))
            values["mtp_ms_runtime_estimate"].append(float(fields["MTP"].removesuffix("ms")))
            values["frm_gpu_ms"].append(float(fields["FrmGpu"].removesuffix("ms")))
            values["atw_gpu_ms"].append(float(fields["ATWGPU"].removesuffix("ms")))
        except (KeyError, ValueError):
            continue
    stats = {k: {"n": len(v), "mean": sum(v) / len(v), "min": min(v), "max": max(v)}
             for k, v in values.items() if v}
    return {"file": str(path), "window": span, "metrics": stats}


if __name__ == "__main__":
    print(json.dumps([parse(Path(arg)) for arg in sys.argv[1:]], indent=2))
