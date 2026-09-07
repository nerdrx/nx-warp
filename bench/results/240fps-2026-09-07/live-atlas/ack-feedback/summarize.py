#!/usr/bin/env python3
"""Summarize this single ACK-bootstrap capture with the parent parser."""
import json
import sys
from pathlib import Path

parent = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(parent))
from summarize_live_atlas import read_pair  # noqa: E402

here = Path(__file__).resolve().parent
out = read_pair(here / "measure-filtered.log", here / "server-filtered.log")
(here / "summary-recomputed.json").write_text(json.dumps(out, indent=2) + "\n")
print(json.dumps(out, indent=2))
