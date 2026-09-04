#!/usr/bin/env python3
"""Render the PAPER section 3.4 table from a nxwarp-phase0 result JSON.

Applies the thresholds and the decision rule and exits non-zero if any
non-informational kernel failed, so CI can gate on it.
"""
import json
import sys

ORDER = ["K1 copy", "K2 gather-4", "K2b sampler", "K3 idct", "K4 rans", "K5 full", "K6 hybrid"]


def fmt_threshold(k):
    if k["threshold_gbps"] > 0:
        return ">= %g GB/s" % k["threshold_gbps"]
    if k["threshold_p50_ms"] > 0 and k["threshold_p99_ms"] > 0:
        return "< %g / %g ms" % (k["threshold_p50_ms"], k["threshold_p99_ms"])
    if k["threshold_p50_ms"] > 0:
        return "< %g ms" % k["threshold_p50_ms"]
    return "informational"


def status(k):
    if not k["ran"]:
        return "SKIP"
    if k["threshold_gbps"] > 0:
        return "PASS" if k["gb_per_sec"] >= k["threshold_gbps"] else "FAIL"
    if k["threshold_p50_ms"] <= 0:
        return "info"
    ok = k["p50_ms"] < k["threshold_p50_ms"]
    if k["threshold_p99_ms"] > 0 and k["p99_ms"] >= k["threshold_p99_ms"]:
        ok = False
    return "PASS" if ok else "FAIL"


def main(path):
    with open(path) as f:
        d = json.load(f)

    dev = d["device"]
    cfg = d["config"]

    print()
    print("NX Warp Phase 0 gate -- PAPER section 3.4")
    print("device      : %s%s" % (dev["name"], (" (%s)" % dev["driver"]) if dev["driver"] else ""))
    print("platform    : %s   mode: %s" % (d["platform"], d["mode"]))
    if d.get("label"):
        print("label       : %s" % d["label"])
    print("subgroup    : size %d (min %d, max %d), ballot %s, size control %s" % (
        dev["subgroup_size"], dev["subgroup_min"], dev["subgroup_max"],
        "yes" if dev["subgroup_ballot"] else "NO",
        "yes" if dev["subgroup_size_control"] else "no"))
    print("shared mem  : %d B   timestampPeriod %g ns   validBits %d" % (
        dev["max_shared_memory"], dev["timestamp_period_ns"], dev["timestamp_valid_bits"]))
    print("frame       : %dx%d   co-tenant reprojection %s at %dx%d" % (
        cfg["width"], cfg["height"], "ON" if cfg["cotenant"] else "off",
        cfg["repro_width"], cfg["repro_height"]))
    print("frames      : %d warm-up + %d measured, per kernel" % (cfg["warmup"], cfg["frames"]))
    print()

    print("  kernel        p50      p95      p99      min      max   threshold      result")
    print("  ----------------------------------------------------------------------------")

    by_name = {k["name"]: k for k in d["kernels"]}
    failed = []
    for name in ORDER:
        k = by_name.get(name)
        if k is None:
            continue
        st = status(k)
        if st == "FAIL":
            failed.append(name)
        if not k["ran"]:
            print("  %-12s %43s   %-14s %s" % (name, "", fmt_threshold(k),
                                             "skip: " + k.get("skip_reason", "")))
            continue
        print("  %-12s %7.3f  %7.3f  %7.3f  %7.3f  %7.3f   %-14s %s" % (
            name, k["p50_ms"], k["p95_ms"], k["p99_ms"], k["min_ms"], k["max_ms"],
            fmt_threshold(k), st))
        if k["gb_per_sec"] > 0:
            print("  %-12s %.1f GB/s achievable" % ("", k["gb_per_sec"]))

    print("  (all times in milliseconds, VK_QUERY_TYPE_TIMESTAMP pairs, timestampPeriod applied)")
    print()

    if "hybrid_decode_latency_p50_ms" in d:
        lat = d["hybrid_decode_latency_p50_ms"]
        print("hybrid base decode latency p50: %.2f ms  (threshold < 15 ms)  %s" % (
            lat, "PASS" if lat < 15.0 else "FAIL"))
        print()

    thermal = [k for k in d["kernels"] if k.get("minute_p50_ms")]
    if thermal:
        print("thermal (per-minute p50, ms)")
        for k in thermal:
            drift = 0.0
            if k["first_minute_p50_ms"] > 0:
                drift = (k["last_minute_p50_ms"] - k["first_minute_p50_ms"]) \
                        / k["first_minute_p50_ms"] * 100.0
            print("  %s: %s" % (k["name"], " ".join("%.2f" % v for v in k["minute_p50_ms"])))
            print("    first %.2f -> last %.2f  (%+.1f%%)" % (
                k["first_minute_p50_ms"], k["last_minute_p50_ms"], drift))
        print()

    print("verdict: %s" % d["verdict"])
    print()

    if failed:
        print("FAILED thresholds: %s" % ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: report.py <result.json>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
