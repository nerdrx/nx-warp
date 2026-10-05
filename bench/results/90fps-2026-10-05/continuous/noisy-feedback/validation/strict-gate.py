import csv
from pathlib import Path
root=Path(__file__).resolve().parents[1]
D=root.parent/"material-recovery-bound/raw"
assert (root/"gates-E/capacity/fall300-clean.csv").read_bytes()==(D/"capacity/fall300-clean.csv").read_bytes(), "E fails byte-exact 300 Mbps collapse control (only <=47 bps target rounding)"
