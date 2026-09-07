#!/usr/bin/env python3
"""Recompute the view-only capture with the shared live-atlas parser."""
import json
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here.parent))
from summarize_live_atlas import read_pair  # noqa: E402

out = read_pair(here / "measure-filtered.log", here / "server-filtered.log")
(here / "summary-recomputed.json").write_text(json.dumps(out, indent=2) + "\n")
print(json.dumps(out, indent=2))
