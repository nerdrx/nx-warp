import csv
from pathlib import Path

root = Path(__file__).resolve().parent
modes = ("baseline", "exploratory-A", "confirmed-B", "growth-bounded-C")

def rows(mode, group, filename="rows.csv"):
    with (root / "raw" / mode / group / filename).open() as f:
        return list(csv.DictReader(f))

phase = {m: rows(m, "phase") for m in modes}
for data in phase.values():
    assert len(data) == 48 * 270
    assert {int(r["burst_frame"]) for r in data} == set(range(-1, 47))
overshoots = {m: {int(r["burst_frame"]) for r in data if int(r["bitrate_bps"]) > 24_000_000}
              for m, data in phase.items()}
assert len(overshoots["exploratory-A"]) == 20
assert not overshoots["baseline"] and not overshoots["confirmed-B"]
assert not overshoots["growth-bounded-C"]

summary = []
for scenario, step in (("rise3-clean", 3), ("rise5-clean", 5), ("rise10-clean", 10), ("rise20-clean", 20)):
    times = {}
    for mode in modes:
        data = rows(mode, "capacity", scenario + ".csv")
        assert len(data) == 3600
        post = [r for r in data if step * 1e9 <= int(r["start_ns"]) < (step + 5) * 1e9]
        recovered = next(int(r["start_ns"]) / 1e9 for r in post if int(r["bitrate_bps"]) >= 840_000_000)
        times[mode] = recovered
        summary.append((scenario, mode, min(int(r["bitrate_bps"]) for r in post), recovered))
    if step in (3, 10):
        assert times["confirmed-B"] < times["baseline"]
    else:
        assert times["confirmed-B"] == times["baseline"]
    if step == 20:
        assert times["growth-bounded-C"] - times["baseline"] > 3

for scenario in ("fall300-clean", "fall250-clean"):
    a = (root / "raw/baseline/capacity" / (scenario + ".csv")).read_bytes()
    b = (root / "raw/confirmed-B/capacity" / (scenario + ".csv")).read_bytes()
    assert a == b

probe = {m: rows(m, "probe-phase", "summary.csv") for m in ("baseline", "confirmed-B", "growth-bounded-C")}
for data in probe.values():
    assert len(data) == 48
for offset in range(38, 47):
    item = lambda mode: next(r for r in probe[mode] if int(r["burst_offset"]) == offset)
    assert int(item("confirmed-B")["phase_max_bps"]) == 50_000_000
    assert int(item("baseline")["phase_max_bps"]) == 20_400_001
    assert int(item("growth-bounded-C")["phase_max_bps"]) <= 26_400_002

for mode in modes:
    data = rows(mode, "adversarial")
    assert len(data) == 6 * 900
    silent = [r for r in data if r["scenario"] == "no-feedback"]
    assert len({r["bitrate_bps"] for r in silent}) == 1
    assert len({r["estimate_bps"] for r in silent}) == 1
with (root / "recovery-summary.csv").open("w", newline="") as f:
    writer = csv.writer(f, lineterminator="\n")
    writer.writerow(("scenario", "mode", "post_step_min_bps", "first_840Mbps_at_virtual_s"))
    writer.writerows(summary)
print("Retained evidence checks pass. A fails steady outliers; B fails probe outliers; C delays recovery. All candidates held; component model only.")
