#!/usr/bin/env python3
"""Task 3(b): server-side ASTC encode cost with ARM astcenc v5.7.0 (AVX2).

All runs are wrapped in the mandated CPU discipline:
    chrt -i 0 taskset -c 0-7 nice -n 19
so at most 8 cores of the 32-core host are ever used, at SCHED_IDLE.

Two per-tile numbers are produced, as they differ a lot:
  * "batch"    : one 1088x1088 (=289 tiles) encode, divided by 289.
                 Reported both as astcenc's own "Coding time" (pure codec,
                 excludes image load/save and process startup) and as wall time.
  * "per-invocation": a single 64x64 image encoded as its own process, wall time.
"""
import json, re, statistics, subprocess, time

A = "/run/media/nerdrx/Lex/claude/nx-scratch/texnative-host/tools/astc-encoder/build/Source/astcenc-avx2"
D = "/run/media/nerdrx/Lex/claude/nx-scratch/texnative-host"
PREFIX = ["chrt", "-i", "0", "taskset", "-c", "0-7", "nice", "-n", "19"]

CODING = re.compile(r"Coding time:\s+([0-9.]+)\s*s")
TOTAL = re.compile(r"Total time:\s+([0-9.]+)\s*s")


def run(src, block, jobs, out="/tmp/tn_t.astc"):
    cmd = PREFIX + [A, "-cl", src, out, block, "-medium", "-j", str(jobs)]
    t0 = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, text=True)
    wall = time.perf_counter() - t0
    assert p.returncode == 0, p.stderr
    return wall, float(CODING.search(p.stdout).group(1)), float(TOTAL.search(p.stdout).group(1))


def stats(v):
    return {
        "min_ms": min(v) * 1000,
        "median_ms": statistics.median(v) * 1000,
        "mean_ms": statistics.fmean(v) * 1000,
        "max_ms": max(v) * 1000,
    }


def main():
    res = {}
    # --- batch: full 1088x1088 luma frame = 289 tiles
    for block in ("4x4", "6x6", "8x8"):
        for jobs in (1, 8):
            walls, codes = [], []
            reps = 5
            for _ in range(reps):
                w, c, _ = run(f"{D}/img/full_luma.png", block, jobs)
                walls.append(w)
                codes.append(c)
            key = f"batch_{block}_j{jobs}"
            res[key] = {
                "reps": reps,
                "wall": stats(walls),
                "coding": stats(codes),
                "coding_ms_per_tile": statistics.median(codes) * 1000 / 289,
                "wall_ms_per_tile": statistics.median(walls) * 1000 / 289,
            }
            print(key, res[key]["coding_ms_per_tile"], flush=True)

    # --- per-invocation: one 64x64 tile as its own process
    for block, src in (("4x4", "tile64.png"), ("6x6", "tile66.png"), ("8x8", "tile64.png")):
        for jobs in (1, 8):
            walls, codes = [], []
            reps = 40
            for _ in range(reps):
                w, c, _ = run(f"{D}/img/{src}", block, jobs)
                walls.append(w)
                codes.append(c)
            key = f"single_{block}_j{jobs}"
            res[key] = {"reps": reps, "wall": stats(walls), "coding": stats(codes)}
            print(key, res[key]["wall"]["median_ms"], res[key]["coding"]["median_ms"], flush=True)

    with open(f"{D}/task3_timing.json", "w") as fh:
        json.dump(res, fh, indent=2)


if __name__ == "__main__":
    main()
