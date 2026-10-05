import csv
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with (root/"recovery.csv").open() as f:rows=list(csv.DictReader(f))
assert all(float(r["F_first_840Mbps_at_s"])<=float(r["D_first_840Mbps_at_s"]) for r in rows), "F delays clean capacity recovery; do not integrate"
