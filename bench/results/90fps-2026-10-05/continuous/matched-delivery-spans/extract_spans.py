#!/usr/bin/env python3
"""Describe exact feedback spans from an opt-in dump; not a controller replay."""
import argparse
import csv
from pathlib import Path
from collections import defaultdict


def union(intervals):
    total = 0
    first = last = None
    for a, b in sorted(intervals):
        if first is None:
            first, last = a, b
        elif a > last:
            total += last - first
            first, last = a, b
        else:
            last = max(last, b)
    return total + (last - first if first is not None else 0)


def extract(reader):
    frames = defaultdict(lambda: {"bytes": defaultdict(int), "receive": {}, "send": {}, "lost": False})
    for number, row in enumerate(reader, 1):
        if not row or row[0] not in ("frame_bytes", "feedback_spans"):
            continue
        expected = 5 if row[0] == "frame_bytes" else 9
        if len(row) != expected:
            raise ValueError(f"line {number}: {row[0]} needs {expected} fields")
        frame, _, stream = map(int, row[1:4])
        if not 0 <= stream < 3:
            continue
        state = frames[frame]
        if row[0] == "frame_bytes":
            count = int(row[4])
            if count < 0:
                raise ValueError(f"line {number}: negative byte count")
            state["bytes"][stream] += count
            continue
        sb, se, rb, re, complete = map(int, row[4:])
        if not complete:
            state["lost"] = True
            continue
        # Receive merge follows D; send merge accepts complete ordered pairs only.
        if rb or re:
            a, b = state["receive"].get(stream, (0, 0))
            state["receive"][stream] = (min(a, rb) if a and rb else a or rb, max(b, re))
        if sb > 0 and se > sb:
            a, b = state["send"].get(stream, (sb, se))
            state["send"][stream] = (min(a, sb), max(b, se))
    for frame, state in sorted(frames.items()):
        streams = [i for i, count in state["bytes"].items() if count and i in state["receive"] and state["receive"][i][0] > 0 and state["receive"][i][1] > state["receive"][i][0]]
        received = [state["receive"][i] for i in streams]
        amount = sum(state["bytes"][i] for i in streams)
        recv = union(received)
        all_send = bool(streams) and all(i in state["send"] for i in streams)
        sent = union([state["send"][i] for i in streams]) if all_send else 0
        valid = bool(recv and amount and not state["lost"])
        yield dict(frame=frame, matched_streams=len(streams), unmatched_byte_streams=sum(bool(n) and i not in streams for i, n in state["bytes"].items()), matched_bytes=amount, lost=int(state["lost"]), all_send_valid=int(all_send), receive_union_ns=recv, receive_max_ns=max((b-a for a,b in received), default=0), send_union_ns=sent, receive_rate_bps=8e9*amount/recv if valid else 0, max_span_rate_bps=8e9*amount/max(recv,sent) if valid else 0)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("input");p.add_argument("output")
    args=p.parse_args()
    if Path(args.input).resolve() == Path(args.output).resolve():
        p.error("input and output must differ")
    with open(args.input, newline="") as f:
        rows=list(extract(csv.reader(f)))
    if not rows:
        p.error("no frame_bytes/feedback_spans events; exact diagnostic capture required")
    with open(args.output,"w",newline="") as f:
        w=csv.DictWriter(f,fieldnames=rows[0].keys(),lineterminator="\n");w.writeheader();w.writerows(rows)

if __name__ == "__main__":
    main()
