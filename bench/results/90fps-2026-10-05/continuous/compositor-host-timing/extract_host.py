#!/usr/bin/env python3
"""Extract PC host intervals from opt-in WiVRn timing CSV; no GPU/latency inference."""
import argparse
import csv
import io
import math
from pathlib import Path

STAGES = ("retirement_poll", "acquire", "record", "queue_lock", "submit", "encoder_present", "timeline_wait", "query_wait", "gc")
EVENTS = {"compositor_" + name for name in STAGES}
OUTCOMES = {"submitted", "timeline_timeout", "retirement_timeout", "no_image"}


def extract(rows):
    result = []
    seen = set()
    for line, row in enumerate(rows, 1):
        if not row or row[0] not in EVENTS:
            continue
        if len(row) != 6:
            raise ValueError(f"line {line}: host row requires six fields")
        event, frame, end, stream, begin, outcome = row
        frame, begin, end, stream = int(frame), int(begin), int(end), int(stream)
        if frame < 0 or frame > 2**64 - 1 or stream != 255 or begin <= 0 or end < begin or outcome not in OUTCOMES:
            raise ValueError(f"line {line}: invalid host interval metadata")
        key = (frame, event)
        if key in seen:
            raise ValueError(f"line {line}: duplicate frame/stage")
        seen.add(key)
        result.append((frame, event.removeprefix("compositor_"), begin, end, end - begin, outcome))
    return result


def percentile(values, fraction):
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)]


def self_check():
    # Synthetic parser fixtures, never observed runtime timings.
    def row(event="compositor_record", frame="7", end="300", stream="255", begin="100", outcome="submitted"):
        return [event, frame, end, stream, begin, outcome]
    assert extract([row()]) == [(7, "record", 100, 300, 200, "submitted")]
    assert extract([row(event="encode_begin")]) == []
    assert extract([row(outcome="no_image", event="compositor_acquire")])[0][-1] == "no_image"
    assert len(extract([row("compositor_" + stage) for stage in STAGES])) == 9
    assert extract([row(end="100")])[0][4] == 0
    for rows in ([row(), row()], [row(stream="0")], [row(begin="0")], [row(end="99")], [row(outcome="unknown")], [row(frame="-1")], [row(frame=str(2**64))], [row()[:5]], [row(begin="oops")]):
        try:
            extract(rows)
        except ValueError:
            pass
        else:
            raise AssertionError(rows)
    assert percentile([1, 2, 3, 4], .50) == 2
    assert percentile([1, 2, 3, 4], .95) == 4
    text = io.StringIO()
    csv.writer(text).writerow(row())
    assert extract(csv.reader(io.StringIO(text.getvalue())))[0][4] == 200
    print("synthetic parser checks passed; no runtime measurement")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, nargs="?")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if args.self_check:
        self_check()
    if args.input is None:
        if args.self_check:
            return
        parser.error("input is required")
    with args.input.open(newline="") as f:
        rows = extract(csv.reader(f))
    if args.output:
        with args.output.open("w", newline="") as f:
            writer = csv.writer(f, lineterminator="\n")
            writer.writerow(("frame", "stage", "begin_pc_ns", "end_pc_ns", "host_duration_ns", "outcome"))
            writer.writerows(rows)
    print(f"{len(rows)} host intervals; absent stages are missing, not zero")
    print("stage,outcome,n,p50_ms,p95_ms,p99_ms")
    for stage in STAGES:
        for outcome in sorted(OUTCOMES):
            values = [r[4] for r in rows if r[1] == stage and r[5] == outcome]
            if values:
                print(f"{stage},{outcome},{len(values)}," + ",".join(f"{percentile(values, q) / 1e6:.6f}" for q in (.5, .95, .99)))



if __name__ == "__main__":
    main()
