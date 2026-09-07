#!/usr/bin/env python3
"""Capture a bounded live probe from the one already-connected Android device."""
import argparse
import hashlib
import json
import math
import shutil
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path


def adb_serial():
    adb = shutil.which("adb") or "/home/nerdrx/.local/bin/adb"
    p = subprocess.run([adb, "devices"], text=True, capture_output=True, check=True)
    rows = [x.split() for x in p.stdout.splitlines()[1:] if x.strip()]
    ready = [x[0] for x in rows if len(x) >= 2 and x[1] == "device"]
    if len(ready) != 1:
        raise SystemExit(f"expected exactly one authorized device, found {len(ready)}")
    return adb, ready[0]


def run(adb, *args, output=None):
    return subprocess.run(adb + list(args), check=True, stdout=output,
                          stderr=subprocess.PIPE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--seconds", type=float, default=40.0)
    ap.add_argument("--wake-period", type=float, default=4.0)
    ap.add_argument("--host-workload", default="Unspecified")
    ap.add_argument("--out", type=Path,
                    default=Path(__file__).resolve().parent)
    a = ap.parse_args()
    if a.seconds <= 0 or a.wake_period <= 0:
        ap.error("seconds and wake-period must be positive")
    if not math.isfinite(a.seconds) or not math.isfinite(a.wake_period):
        ap.error("seconds and wake-period must be finite")
    adb_path, serial = adb_serial()
    adb = [adb_path, "-s", serial]
    out = a.out
    out.mkdir(parents=True, exist_ok=True)
    started_wall = datetime.now(timezone.utc).isoformat()
    started = time.monotonic()
    run(adb, "logcat", "-c")
    wakes = []
    host_samples = []
    shots = []
    next_wake = 0.0
    shot_at = {6.0, max(6.0, a.seconds - 6.0)}
    done = started + a.seconds
    while True:
        elapsed = time.monotonic() - started
        if elapsed >= a.seconds:
            break
        if elapsed >= next_wake:
            run(adb, "shell", "input", "keyevent", "KEYCODE_WAKEUP")
            wakes.append({"elapsed_s": time.monotonic() - started})
            sample = {"elapsed_s": time.monotonic() - started, "gpus": []}
            for busy in sorted(Path("/sys/class/drm").glob("card*/device/gpu_busy_percent")):
                gpu = {"device_path": str(busy.parent.resolve())}
                for key, path in [("busy_percent", busy), ("device_id", busy.parent / "device")]:
                    try:
                        gpu[key] = path.read_text().strip()
                    except OSError:
                        gpu[key] = None
                gpu["temperatures_millidegrees_c"] = {}
                gpu["temperature_labels"] = {}
                for temp in busy.parent.glob("hwmon/hwmon*/temp*_input"):
                    try:
                        gpu["temperatures_millidegrees_c"][str(temp)] = int(temp.read_text())
                        label = temp.with_name(temp.name.replace("_input", "_label"))
                        if label.is_file():
                            gpu["temperature_labels"][str(temp)] = label.read_text().strip()
                    except (OSError, ValueError):
                        pass
                sample["gpus"].append(gpu)
            host_samples.append(sample)
            next_wake += a.wake_period
            while next_wake <= time.monotonic() - started:
                next_wake += a.wake_period
            continue
        due = [x for x in shot_at if x <= elapsed]
        for target in sorted(due):
            path = out / f"{a.name}-screen-{int(target):02d}.png"
            with path.open("wb") as f:
                run(adb, "exec-out", "screencap", "-p", output=f)
            shots.append({"target_s": target, "elapsed_s": time.monotonic() - started,
                          "path": str(path),
                          "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
            shot_at.remove(target)
        time.sleep(min(0.05, max(0.0, done - time.monotonic())))
    log_path = out / f"{a.name}-measure.log"
    with log_path.open("w", encoding="utf-8") as f:
        run(adb, "logcat", "-d", "-v", "threadtime", "-s", "WiVRn", output=f)
    ended = time.monotonic()
    manifest = {
        "name": a.name, "serial": serial, "started_utc": started_wall,
        "requested_seconds": a.seconds, "actual_elapsed_s": ended - started,
        "wake_period_s": a.wake_period, "wakes": wakes, "screenshots": shots,
        "host_samples": host_samples,
        "host_workload": a.host_workload,
        "measure_log": str(log_path),
    }
    (out / f"{a.name}-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
