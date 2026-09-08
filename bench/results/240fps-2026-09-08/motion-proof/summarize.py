#!/usr/bin/env python3
"""Summarize retained motion-proof evidence without unpacking its tar archive."""
import csv
import gzip
import io
import json
import statistics
import math
import sys
import tarfile
from pathlib import Path

PERIOD_MS = 1000.0 / 240.0


def read_members(archive):
    with tarfile.open(archive, "r") as tar:
        members = {Path(m.name).name: m for m in tar.getmembers()}
        csv_member = members["pan-proof.csv.gz"]
        raw = tar.extractfile(csv_member).read()
        rows = list(csv.DictReader(io.TextIOWrapper(gzip.GzipFile(fileobj=io.BytesIO(raw)), encoding="utf-8")))
        manifest = json.load(io.TextIOWrapper(tar.extractfile(members["pan-7200.manifest.json"])))
        camera_manifest = json.load(io.TextIOWrapper(tar.extractfile(members["camera-parallel.manifest.json"])))
        log = tar.extractfile(members["pan-proof.log"]).read().decode()
        camera_log = tar.extractfile(members["camera-proof.log"]).read().decode()
        repeat_log = tar.extractfile(members["camera-repeat.log"]).read().decode()
        device_sha256 = tar.extractfile(members["device-sha256.txt"]).read().decode()
        camera_sha256 = tar.extractfile(members["camera-build-sha256.txt"]).read().decode()
        camera_device_sha256 = tar.extractfile(members["camera-device-sha256.txt"]).read().decode()
        device_state = tar.extractfile(members["device-state-before-camera.txt"]).read().decode()
        def camera_rows(name):
            raw = tar.extractfile(members[name]).read()
            return list(csv.DictReader(io.TextIOWrapper(gzip.GzipFile(fileobj=io.BytesIO(raw)), encoding="utf-8")))
        camera = camera_rows("camera-proof.csv.gz")
        repeat = camera_rows("camera-repeat.csv.gz")
    return (rows, manifest, log, device_sha256, camera_manifest, camera_log, repeat_log,
            camera_sha256, camera_device_sha256, device_state, camera, repeat)


def stats(rows):
    values = [float(row["total_ms"]) for row in rows]
    if not values or any(not math.isfinite(v) or v < 0 for v in values):
        raise SystemExit("invalid timing samples")
    q = statistics.quantiles(values, n=100, method="inclusive")
    return {
        "count": len(values),
        "p50_ms": q[49],
        "p95_ms": q[94],
        "p99_ms": q[98],
        "max_ms": max(values),
        "deadline_ms": PERIOD_MS,
        "deadline_misses": sum(value > PERIOD_MS for value in values),
    }


def main():
    archive = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name("input-evidence.tar")
    (rows, manifest, log, device_sha256, camera_manifest, camera_log, repeat_log,
     camera_sha256, camera_device_sha256, device_state, camera, repeat) = read_members(archive)
    if len(rows) != 7200 or [int(r["frame"]) for r in rows] != list(range(7200)):
        raise SystemExit("expected exactly 7200 ordered CSV rows")
    if manifest.get("frames") != 7200 or manifest.get("distinct_source_frames") != 7200:
        raise SystemExit("manifest does not identify 7200 distinct source frames")
    hashes = {frame["sha256"] for frame in manifest["source_frames"]}
    if len(hashes) != 7200:
        raise SystemExit("source-frame hashes are not all distinct")
    device_hashes = dict((line.split()[1], line.split()[0])
                         for line in device_sha256.splitlines() if line.strip())
    stream_path = "/data/local/tmp/planar-direct/pan-7200.nxv"
    if device_hashes.get(stream_path) != manifest["stream_sha256"]:
        raise SystemExit("device stream hash differs from source manifest")
    camera_hashes = {frame["sourcehash"] for frame in camera_manifest["source_frames"]}
    if len(camera) != camera_manifest.get("frames") or len(camera_hashes) != camera_manifest.get("distinct_source_frames"):
        raise SystemExit("camera evidence rows or source hashes do not match manifest")
    if len(camera_hashes) != 720 or camera_manifest.get("fps") != 240.0:
        raise SystemExit("expected 720 distinct sources on a 240 FPS camera trajectory")
    for arm in (camera, repeat):
        if [int(r["frame"]) for r in arm] != list(range(720)):
            raise SystemExit("camera run must contain 720 ordered frame rows")
    camera_device_hashes = {Path(line.split()[1]).name: line.split()[0]
                            for line in camera_device_sha256.splitlines() if line.strip()}
    build_hashes = {Path(line.split()[1]).name: line.split()[0]
                    for line in camera_sha256.splitlines() if line.strip()}
    if camera_device_hashes.get("camera.nxv") != camera_manifest["streamhash"]:
        raise SystemExit("camera device input differs from source manifest")
    for device_name, build_name in [("nx-planar-camera", "nx-planar-direct"),
                                    ("tile.vert.spv", "tile.vert.spv"),
                                    ("tile.frag.spv", "tile.frag.spv")]:
        if camera_device_hashes.get(device_name) != build_hashes.get(build_name):
            raise SystemExit("device executable/shader differs from build")
    result = {
        "all_7200": stats(rows),
        "warm_24_plus": stats(rows[24:]),
        "source_frames": {
            "manifest_frames": manifest["frames"],
            "distinct_source_frames": manifest["distinct_source_frames"],
            "stream_sha256": manifest.get("stream_sha256"),
        },
        "device_manifest": {
            "stream_hash_matches_source_manifest": True,
            "device_file_sha256": device_hashes,
            "log_identity_verified": "device=Adreno (TM) 650" in log,
        },
        "camera_3s": {
            "camera_proof": stats(camera),
            "camera_repeat": stats(repeat),
            "camera_proof_warm24": stats(camera[24:]),
            "camera_repeat_warm24": stats(repeat[24:]),
            "device_file_sha256": camera_device_hashes,
            "manifest_frames": camera_manifest["frames"],
            "distinct_source_frames": camera_manifest["distinct_source_frames"],
            "streamhash": camera_manifest["streamhash"],
            "device_state_present": bool(device_state.strip()),
            "camera_log_identity_verified": "device=Adreno (TM) 650" in camera_log,
            "repeat_log_identity_verified": "device=Adreno (TM) 650" in repeat_log,
            "build_sha256_present": bool(camera_sha256.strip()),
            "device_input_hash_matches_streamhash": True,
        },
        "deadline_definition": "total_ms > 1000/240; total_ms is probe scheduled-to-completion timing",
    }
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
