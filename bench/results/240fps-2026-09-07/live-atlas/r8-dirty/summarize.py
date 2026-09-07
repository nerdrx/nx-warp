#!/usr/bin/env python3
"""Recompute the preliminary R8 full/dirty pair from local filtered logs."""
import json
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here.parent))
from summarize_live_atlas import read_pair  # noqa: E402

out = {
    "full": read_pair(here / "full-measure-filtered.log", here / "full-server-filtered.log"),
    "full_repeat": read_pair(here / "full-repeat-measure-filtered.log", here / "full-repeat-server-filtered.log"),
    "dirty": read_pair(here / "dirty-measure-filtered.log", here / "dirty-server-filtered.log"),
}
(here / "summary-recomputed.json").write_text(json.dumps(out, indent=2) + "\n")
print(json.dumps(out, indent=2))
