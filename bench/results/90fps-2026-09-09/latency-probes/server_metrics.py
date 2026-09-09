#!/usr/bin/env python3
"""Summarize the last 30 server encode windows from supplied log files."""
import json
import re
import sys
from pathlib import Path

LINE = re.compile(
    r"encoded\s+(\d+)\s+frames\s+in\s+([0-9.]+)\s+s:\s+"
    r"([0-9.]+)\s+ms/frame\s+\(max\s+([0-9.]+)\),\s+"
    r"([0-9.]+)\s+B/frame.*?"
    r"QP\s+([0-9.]+)\s+\[([0-9.]+)\.\.([0-9.]+)\].*?"
    r"controller allows\s+([0-9.]+)\s+Mbit/s.*?"
    r"paced to\s+([0-9.]+)\s+fps\s+\(client decode\s+([0-9.]+)\s+ms\)")


def parse(path):
    rows = []
    for line in path.read_text(errors="replace").splitlines():
        match = LINE.search(line)
        if not match:
            continue
        (frames, seconds, enc, enc_max, bytes_frame, qp, qp_min, qp_max,
         allowed, paced, decode) = match.groups()
        frames, seconds, bytes_frame = int(frames), float(seconds), float(bytes_frame)
        actual = frames / seconds
        rows.append({"encoded_fps": actual, "encode_ms": float(enc),
                     "encode_max_ms": float(enc_max), "bytes_per_frame": bytes_frame,
                     "qp_mean": float(qp), "qp_min": float(qp_min),
                     "qp_max": float(qp_max),
                     "payload_mbit_s": 8 * bytes_frame * actual / 1e6,
                     "controller_allows_mbit_s": float(allowed),
                     "paced_target_fps": float(paced), "client_decode_ms": float(decode)})
    recent = rows[-30:]
    fields = tuple(recent[0]) if recent else ()
    means = {field: sum(row[field] for row in recent) / len(recent) for field in fields}
    return {"file": str(path), "raw_row_count": len(rows), "last30_count": len(recent),
            "last30_means": means}


if __name__ == "__main__":
    print(json.dumps([parse(Path(arg)) for arg in sys.argv[1:]], indent=2))
