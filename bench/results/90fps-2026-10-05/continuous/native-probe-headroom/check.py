from pathlib import Path
import csv, sys
root = Path(sys.argv[1])
rows = {}
for mode in ("baseline", "candidate"):
    for case in ("clean", "early", "late", "late-one-eye"):
        data = list(csv.DictReader((root / f"{mode}-{case}.csv").open()))
        assert len(data) == 3600
        for i, r in enumerate(data):
            assert int(r["frame"]) == i
            assert int(r["lag_ns"]) >= 0
            assert int(r["serial_ns"]) == int(r["eye0_span_ns"]) + int(r["eye1_span_ns"])
        rows[mode, case] = data
        window = [r for r in data if 6e9 <= int(r["desired_ns"]) < 20e9]
        recovery = [r for r in data if int(r["desired_ns"]) >= 20e9]
        probes = sum(int(r["probe_entry"]) for r in window)
        resumed = [r for r in recovery if int(r["probe_entry"])]
        assert resumed
        if case.startswith("late"):
            assert probes == (2 if mode == "baseline" else 0)
            if mode == "candidate":
                assert max(int(r["bitrate_bps"]) for r in window) < 500_000_000
                assert int(data[-1]["bitrate_bps"]) >= 840_000_000
        print(f"{mode}/{case}: stress_probes={probes}, stress_peak_mbps={max(int(r['bitrate_bps']) for r in window)/1e6:.3f}, first_recovery_probe_s={int(resumed[0]['desired_ns'])/1e9:.3f}, final_mbps={int(data[-1]['bitrate_bps'])/1e6:.3f}")
def dynamics(data):
    return [{k:v for k,v in r.items() if k not in ("label", "feedback_kind")} for r in data]
for case in ("clean", "early"):
    assert dynamics(rows["baseline",case]) == dynamics(rows["candidate",case])
for mode in ("baseline", "candidate"):
    assert dynamics(rows[mode,"clean"]) == dynamics(rows[mode,"early"])
    assert dynamics(rows[mode,"late"]) == dynamics(rows[mode,"late-one-eye"])
print("All 28,800 rows checked; clean/input-boundary dynamics unchanged; one-eye/both-eye dynamics match.")

def first_recovered(mode):
    return next(int(r["desired_ns"])/1e9 for r in rows[mode,"late"] if int(r["desired_ns"])>=20e9 and int(r["bitrate_bps"])>=840_000_000)
baseline_recovery = first_recovered("baseline")
candidate_recovery = first_recovered("candidate")
print(f"Recovery >=840Mbps: baseline={baseline_recovery:.6f}s candidate={candidate_recovery:.6f}s delta={candidate_recovery-baseline_recovery:.6f}s")
assert candidate_recovery > baseline_recovery
print("POLICY GATE: HOLD. Fewer stress probes, but capacity-return transient and 0.5s slower modeled recovery.")
