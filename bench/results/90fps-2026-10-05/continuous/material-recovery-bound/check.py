import csv
from pathlib import Path

root = Path(__file__).resolve().parent
previous = root.parent / "fresh-peak-confirmation"

def load(folder, name):
    with (folder / name).open() as f:
        return list(csv.DictReader(f))

for case in ("fall300-clean", "fall250-clean"):
    assert (root / "raw/capacity" / (case + ".csv")).read_bytes() == (previous / "raw/baseline/capacity" / (case + ".csv")).read_bytes()

summary = []
for case, step in (("rise3-clean", 3), ("rise5-clean", 5), ("rise10-clean", 10), ("rise20-clean", 20)):
    times = {}
    for label, folder in (("baseline", previous / "raw/baseline/capacity"), ("D", root / "raw/capacity")):
        data = load(folder, case + ".csv")
        assert len(data) == 3600
        post = [r for r in data if step * 1e9 <= int(r["start_ns"]) < (step + 5) * 1e9]
        times[label] = next(int(r["start_ns"]) / 1e9 for r in post if int(r["bitrate_bps"]) >= 840_000_000)
        summary.append((case, label, min(int(r["bitrate_bps"]) for r in post), times[label]))
    assert times["D"] < times["baseline"] if step in (3, 10) else times["D"] == times["baseline"]

steady = load(root / "raw/phase", "rows.csv")
assert len(steady) == 48 * 270
assert max(int(r["bitrate_bps"]) for r in steady) <= 24_000_000
probe = load(root / "raw/probe-phase", "summary.csv")
assert len(probe) == 48
control = next(r for r in probe if int(r["burst_offset"]) == -1)
for r in probe:
    assert int(r["phase_max_bps"]) <= int(control["phase_max_bps"]) + 2
# The deliberate probe still exists and drains after its ordinary duration.
old_control = next(r for r in load(previous / "raw/baseline/probe-phase", "summary.csv") if int(r["burst_offset"]) == -1)
assert control == old_control

adversarial = load(root / "raw/adversarial", "rows.csv")
assert len(adversarial) == 6 * 900
silent = [r for r in adversarial if r["scenario"] == "no-feedback"]
assert len({r["bitrate_bps"] for r in silent}) == 1
with (root / "recovery-summary.csv").open("w", newline="") as f:
    writer = csv.writer(f, lineterminator="\n")
    writer.writerow(("scenario", "mode", "post_step_min_bps", "first_840Mbps_at_virtual_s"))
    writer.writerows(summary)
print("Scoped replay gates pass: recovery improves at3/10s;5/20s unchanged; both collapses exact; burst growth bounded.")
