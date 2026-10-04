#!/usr/bin/env python3
"""Run the 10-cycle baseline/pool ABBA loop; executable startup is outside timing."""
import argparse
import csv
import pathlib
import re
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("baseline", type=pathlib.Path)
parser.add_argument("pool", type=pathlib.Path)
parser.add_argument("output", type=pathlib.Path)
parser.add_argument("--cpu", type=int, default=20)
parser.add_argument("--cycles", type=int, default=10)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
runs_dir = args.output / "runs"
runs_dir.mkdir(exist_ok=True)
sequence = [name for _ in range(args.cycles) for name in ("baseline", "pool", "pool", "baseline")]
executables = {"baseline": args.baseline, "pool": args.pool}
rows = []
for run, treatment in enumerate(sequence, 1):
    start = time.monotonic_ns()
    proc = subprocess.run(
        ["taskset", "-c", str(args.cpu), str(executables[treatment])],
        check=True,
        capture_output=True,
        text=True,
    )
    wall_ns = time.monotonic_ns() - start
    stdout_file = f"run_{run:02d}_{treatment}.out"
    (runs_dir / stdout_file).write_text(proc.stdout)
    result = next((line for line in proc.stdout.splitlines() if line.startswith("RESULT ")), None)
    if result is None:
        raise RuntimeError(f"run {run} emitted no RESULT line")
    fields = dict(re.findall(r"(\w+)=([0-9]+)", result))
    rows.append(
        {
            "run": run,
            "abba_cycle": (run - 1) // 4 + 1,
            "abba_position": (run - 1) % 4 + 1,
            "treatment": treatment,
            "matching_allocs": int(fields["matching_allocs"]),
            "receive_ns": int(fields["receive_ns"]),
            "batch_p50_ns": int(fields["per_batch_p50_ns"]),
            "batch_p95_ns": int(fields["per_batch_p95_ns"]),
            "process_wall_ns": wall_ns,
            "stdout_file": stdout_file,
        }
    )
with (args.output / "runs.csv").open("w", newline="") as f:
    writer = csv.DictWriter(f, lineterminator="\n", fieldnames=rows[0].keys())
    writer.writeheader()
    writer.writerows(rows)
