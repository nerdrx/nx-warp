#!/usr/bin/env python3
"""Generate a tiny mock harness from the exact production gate and wait block."""

import pathlib
import sys

repo = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
source = (repo / "server/encoder/video_encoder.cpp").read_text()

constructor_pos = source.index("video_encoder::video_encoder(")
gate_start = source.index(
    '\tif (is_native_astc)\n\t{\n\t\tconst char * timing = std::getenv("WIVRN_ASTC_SENDER_WAIT_TIMING");',
    constructor_pos,
)
gate_end = source.index("\n\t}\n\n\t// Only a hardware encoder", gate_start) + len("\n\t}")
gate = source[gate_start:gate_end]

encode_pos = source.index("void video_encoder::encode(")
wait_start = source.index(
    "\tif (shared_sender)\n\t{\n\t\tif (is_native_astc and astc_sender_wait_timing_enabled)",
    encode_pos,
)
wait_end = source.index("\n\tthis->cnx = &cnx;", wait_start)
wait_block = source[wait_start:wait_end]

template = r'''#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
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
struct fake_sender {
    unsigned calls = 0;
    void wait_idle(void *) { ++calls; }
};

struct harness {
    const bool is_native_astc;
    unsigned stream_idx = 7;
    fake_sender * shared_sender;
    bool astc_sender_wait_timing_enabled = false;
    uint32_t astc_sender_wait_samples = 0;
    uint64_t astc_sender_wait_total_ns = 0;
    uint64_t astc_sender_wait_max_ns = 0;
    bool astc_slot_wait_timing_enabled = false;
    harness(bool native_astc, fake_sender * sender) : is_native_astc(native_astc), shared_sender(sender) {
@CONSTRUCTOR_GATE@
    }
    void measure() {
@WAIT_BLOCK@
    }
};

static void reset_clock() { clock_script.clear(); clock_pos = 0; clock_calls = 0; }
static void add_sample(int64_t & now, int64_t duration) {
    clock_script.push_back(now);
    clock_script.push_back(now + duration);
    now += 20'000'000;
}

int main() {
    unsetenv("WIVRN_ASTC_SLOT_WAIT_TIMING");
    fake_sender sender;

    unsetenv("WIVRN_ASTC_SENDER_WAIT_TIMING");
    reset_clock();
    harness disabled(true, &sender);
    disabled.measure();
    assert(!disabled.astc_sender_wait_timing_enabled && sender.calls == 1);
    assert(clock_calls == 0 && disabled.astc_sender_wait_samples == 0);

    setenv("WIVRN_ASTC_SENDER_WAIT_TIMING", "1", 1);
    reset_clock();
    harness other_codec(false, &sender);
    other_codec.measure();
    assert(!other_codec.astc_sender_wait_timing_enabled && sender.calls == 2);
    assert(clock_calls == 0 && other_codec.astc_sender_wait_samples == 0);

    setenv("WIVRN_ASTC_SENDER_WAIT_TIMING", "1 ", 1);
    reset_clock();
    harness inexact(true, &sender);
    inexact.measure();
    assert(!inexact.astc_sender_wait_timing_enabled && clock_calls == 0);

    setenv("WIVRN_ASTC_SENDER_WAIT_TIMING", "1", 1);
    reset_clock();
    harness no_sender(true, nullptr);
    no_sender.measure();
    assert(no_sender.astc_sender_wait_timing_enabled && clock_calls == 0);
    assert(no_sender.astc_sender_wait_samples == 0);

    reset_clock();
    rows.clear();
    harness enabled(true, &sender);
    assert(enabled.astc_sender_wait_timing_enabled);
    int64_t now = 1'000'000'000;
    for (unsigned i = 0; i < 180; ++i)
        add_sample(now, i == 179 ? 10'000'000 : 0); // Includes 179 idle/zero waits.
    for (unsigned i = 0; i < 180; ++i)
        add_sample(now, i == 179 ? 3'000'000 : 1'000'000);

    for (unsigned i = 0; i < 179; ++i) enabled.measure();
    assert(rows.empty() && enabled.astc_sender_wait_samples == 179);
    enabled.measure();
    assert(rows.size() == 1 && rows[0].stream == 7 && rows[0].n == 180);
    assert(std::abs(rows[0].mean_ms - (10.0 / 180.0)) < 0.000001);
    assert(std::abs(rows[0].max_ms - 10.0) < 0.000001);
    assert(enabled.astc_sender_wait_samples == 0 && enabled.astc_sender_wait_total_ns == 0 &&
           enabled.astc_sender_wait_max_ns == 0);

    for (unsigned i = 0; i < 180; ++i) enabled.measure();
    assert(rows.size() == 2 && rows[1].n == 180 && rows[1].stream == 7);
    assert(std::abs(rows[1].mean_ms - (182.0 / 180.0)) < 0.000001);
    assert(std::abs(rows[1].max_ms - 3.0) < 0.000001);
    assert(enabled.astc_sender_wait_samples == 0 && enabled.astc_sender_wait_total_ns == 0 &&
           enabled.astc_sender_wait_max_ns == 0);
    assert(clock_calls == 720);
    assert(sender.calls == 363);
    std::puts("PASS: extracted production gate/wait block; disabled gates, zero waits, 180-window mean/max/reset");
}
'''

out.mkdir(parents=True, exist_ok=True)
generated = template.replace("@CONSTRUCTOR_GATE@", gate).replace("@WAIT_BLOCK@", wait_block)
(out / "extracted-check.cpp").write_text(generated)
print(f"Extracted constructor gate: {len(gate)} bytes; wait block: {len(wait_block)} bytes")
