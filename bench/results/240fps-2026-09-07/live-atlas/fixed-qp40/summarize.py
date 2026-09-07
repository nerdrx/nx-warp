#!/usr/bin/env python3
"""Summarize matched fixed-QP40 pace-45 logs with the shared parser."""
import json
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here.parent))
from summarize_live_atlas import read_pair  # noqa: E402

out = {
    "atlas": read_pair(here / "atlas-measure-filtered.log", here / "atlas-server-filtered.log"),
    "off": read_pair(here / "off-measure-filtered.log", here / "off-server-filtered.log"),
}
(here / "summary-recomputed.json").write_text(json.dumps(out, indent=2) + "\n")
print(json.dumps(out, indent=2))
