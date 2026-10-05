#!/usr/bin/env python3
"""Generate a mock check from the exact base constructor gate and slot wait."""

import pathlib
import sys

repo = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
source = (repo / "server/encoder/video_encoder.cpp").read_text()

constructor_pos = source.index("video_encoder::video_encoder(")
gate_start = source.index(
    "\tif (is_native_astc)\n\t{\n\t\tconst char * timing = "
    'std::getenv("WIVRN_ASTC_SENDER_WAIT_TIMING");',
    constructor_pos,
)
gate_end = source.index("\n\t}\n\n\t// Only a hardware encoder", gate_start) + len("\n\t}")
gate = source[gate_start:gate_end]

present_pos = source.index("void video_encoder::present_image(")
wait_start = source.index(
    "\tif (is_native_astc and astc_slot_wait_timing_enabled)\n",
    present_pos,
)
wait_end = source.index("\n\tif (idr->should_skip(frame_index))", wait_start)
wait_block = source[wait_start:wait_end]

template = r'''#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

struct log_row { unsigned stream, n; double mean_ms, max_ms; };
static std::vector<log_row> rows;
static std::vector<int64_t> clock_script;
static size_t clock_pos = 0;
static unsigned clock_calls = 0;
static int64_t os_monotonic_get_ns() {
    ++clock_calls;
    assert(clock_pos < clock_script.size());
    return clock_script[clock_pos++];
}
static void fake_log(const char *, unsigned stream, unsigned n, double mean, double maximum) {
    rows.push_back({stream, n, mean, maximum});
}
#define U_LOG_I(...) fake_log(__VA_ARGS__)
struct fake_state {
    unsigned waits = 0;
    void wait(int) { ++waits; }
};

struct harness {
    const bool is_native_astc;
    unsigned stream_idx = 4;
    static constexpr int busy = 1;
    size_t present_slot = 0;
    std::array<fake_state, 2> state;
    bool astc_sender_wait_timing_enabled = false;
    bool astc_slot_wait_timing_enabled = false;
    uint32_t astc_slot_wait_samples = 0;
    uint64_t astc_slot_wait_total_ns = 0;
    uint64_t astc_slot_wait_max_ns = 0;
    explicit harness(bool native) : is_native_astc(native) {
@CONSTRUCTOR_GATE@
    }
    void present_wait(bool skipped) {
@WAIT_BLOCK@
        if (skipped) return; // Production checks IDR skip after waiting.
    }
};

static void reset_clock() { clock_script.clear(); clock_pos = 0; clock_calls = 0; }
static void add_sample(int64_t & now, int64_t duration) {
    clock_script.push_back(now);
    clock_script.push_back(now + duration);
    now += 20'000'000;
}

int main() {
    unsetenv("WIVRN_ASTC_SENDER_WAIT_TIMING");
    unsetenv("WIVRN_ASTC_SLOT_WAIT_TIMING");
    reset_clock();
    harness disabled(true);
    disabled.present_wait(false);
    assert(!disabled.astc_slot_wait_timing_enabled && disabled.state[0].waits == 1);
    assert(clock_calls == 0 && disabled.astc_slot_wait_samples == 0);

    setenv("WIVRN_ASTC_SLOT_WAIT_TIMING", "1", 1);
    reset_clock();
    harness other_codec(false);
    other_codec.present_wait(true);
    assert(!other_codec.astc_slot_wait_timing_enabled && other_codec.state[0].waits == 1);
    assert(clock_calls == 0 && other_codec.astc_slot_wait_samples == 0);

    setenv("WIVRN_ASTC_SLOT_WAIT_TIMING", "1 ", 1);
    reset_clock();
    harness inexact(true);
    inexact.present_wait(false);
    assert(!inexact.astc_slot_wait_timing_enabled && clock_calls == 0);

    setenv("WIVRN_ASTC_SLOT_WAIT_TIMING", "1", 1);
    reset_clock();
    rows.clear();
    harness enabled(true);
    assert(enabled.astc_slot_wait_timing_enabled);
    int64_t now = 2'000'000'000;
    for (unsigned i = 0; i < 180; ++i)
        add_sample(now, i == 179 ? 10'000'000 : 0);
    for (unsigned i = 0; i < 180; ++i)
        add_sample(now, i == 179 ? 3'000'000 : 1'000'000);

    for (unsigned i = 0; i < 179; ++i)
        enabled.present_wait(i == 0); // A skipped present still passes through the wait.
    assert(rows.empty() && enabled.astc_slot_wait_samples == 179);
    enabled.present_wait(false);
    assert(rows.size() == 1 && rows[0].stream == 4 && rows[0].n == 180);
    assert(std::abs(rows[0].mean_ms - (10.0 / 180.0)) < 0.000001);
    assert(std::abs(rows[0].max_ms - 10.0) < 0.000001);
    assert(enabled.astc_slot_wait_samples == 0 && enabled.astc_slot_wait_total_ns == 0 &&
           enabled.astc_slot_wait_max_ns == 0);

    for (unsigned i = 0; i < 180; ++i)
        enabled.present_wait(false);
    assert(rows.size() == 2 && rows[1].n == 180 && rows[1].stream == 4);
    assert(std::abs(rows[1].mean_ms - (182.0 / 180.0)) < 0.000001);
    assert(std::abs(rows[1].max_ms - 3.0) < 0.000001);
    assert(enabled.astc_slot_wait_samples == 0 && enabled.astc_slot_wait_total_ns == 0 &&
           enabled.astc_slot_wait_max_ns == 0);
    assert(clock_calls == 720 && enabled.state[0].waits == 360);
    std::puts("PASS: extracted constructor gate and slot wait; disabled gates, skipped presents, zero samples, mean/max/reset");
}
'''

if source.index("state[present_slot].wait(busy);", wait_start) > source.index(
    "\tif (idr->should_skip(frame_index))", wait_start
):
    raise SystemExit("slot wait moved after IDR skip check")

out.mkdir(parents=True, exist_ok=True)
generated = template.replace("@CONSTRUCTOR_GATE@", gate).replace("@WAIT_BLOCK@", wait_block)
(out / "extracted-check.cpp").write_text(generated)
print(f"Extracted constructor gate: {len(gate)} bytes; slot wait branch: {len(wait_block)} bytes")
