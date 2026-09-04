// NX Warp Android client -- geometry, runtime configuration, small utilities.
//
// The geometry constants are the v1 defaults of docs/TRANSPORT.md section 1.
// They are *defaults*, not assumptions: everything downstream reads them from a
// StreamConfig so that a different stream geometry (or the 32x32 tile the
// bitstream header reserves, PAPER 6.2) needs no code change.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace nxc {

// ---------------------------------------------------------------- geometry

// docs/TRANSPORT.md section 1, "v1 value" column.
struct StreamConfig {
    uint32_t tile_size = 64;
    uint32_t cols      = 68;   // ceil(4320 / 64)
    uint32_t rows      = 34;   // ceil(2160 / 64)
    uint32_t band_rows = 6;    // PAPER 4.2
    uint32_t bands     = 6;    // last band is 4 rows
    uint32_t ring_slots = 4;   // PAPER 4.3 / 6.6

    uint32_t tiles_per_frame() const { return cols * rows; }          // 2312

    // TRANSPORT.md 1: band = min(row / band_rows, bands - 1)
    uint32_t band_of_row(uint32_t row) const {
        uint32_t b = row / band_rows;
        return b >= bands ? bands - 1 : b;
    }
    uint32_t band_of_tile(uint32_t tile_index) const {
        return band_of_row(tile_index / cols);
    }
    // First tile row of a band, and the number of rows it spans.
    uint32_t band_first_row(uint32_t band) const { return band * band_rows; }
    uint32_t band_row_count(uint32_t band) const {
        uint32_t first = band_first_row(band);
        if (first >= rows) return 0;
        uint32_t last = (band + 1 == bands) ? rows : (first + band_rows);
        if (last > rows) last = rows;
        return last - first;
    }
    uint32_t tiles_in_band(uint32_t band) const { return cols * band_row_count(band); }
    // Bit position of a tile within its band's feedback bitmap (TRANSPORT.md 1).
    uint32_t tile_in_band(uint32_t tile_index) const {
        uint32_t row  = tile_index / cols;
        uint32_t col  = tile_index % cols;
        uint32_t band = band_of_row(row);
        return (row - band_first_row(band)) * cols + col;
    }
};

// ---------------------------------------------------------------- app config

enum class AppMode {
    // Normal client: receive, depacketize, place, present, feed back.
    kReceive,
    // pps/throughput self test: everything above still runs, but the app also
    // consumes the nxvc-blast self-test trailer to get ground-truth loss and it
    // reports pps/bytes/CPU to the blaster and to logcat once a second.
    kSelfTest,
};

struct AppConfig {
    StreamConfig stream;

    uint16_t    listen_port      = 9944;
    // Requested SO_RCVBUF. PAPER 4.11: "Request 8 MB, verify it was granted
    // (vendor ROMs cap rmem_max)". The granted size is logged and shown on the HUD.
    int         want_rcvbuf      = 8 * 1024 * 1024;
    // recvmmsg batch. PAPER 4.1 / 4.11: the batch is what buys the headroom
    // between 50k and 100k pps.
    int         recv_batch       = 64;
    // Datagram slab: slots x slot_bytes of pinned host memory that recvmmsg
    // writes into directly. 16384 x 9216 = 144 MB is too much for a phone; the
    // default sizes for a 1400-byte MTU with room for a jumbo probe.
    uint32_t    ring_slots       = 8192;      // power of two, see SpscRing
    uint32_t    slot_bytes       = 1536;      // >= mtu; jumbo mode raises this
    // Attempt SCHED_FIFO on the receive thread, and pin it to a big core.
    bool        want_sched_fifo  = true;
    int         sched_fifo_prio  = 10;
    bool        want_affinity    = true;

    // Presentation
    uint32_t    display_hz       = 90;
    // PAPER 4.3 item 4: wake at predicted_display - reproject_budget - runtime_margin.
    uint32_t    reproject_budget_us = 1500;
    uint32_t    runtime_margin_us   = 3000;   // "about 3 ms on Pico"

    bool        send_feedback    = true;
    bool        hud              = true;
    AppMode     mode             = AppMode::kSelfTest;

    // NullAead-equivalent payload touch. The stub transport does not do real
    // AEAD (see nxc_transport.h); with this on it still *reads* every payload
    // byte so that the pps measurement includes the memory traffic a real
    // AES-256-GCM open would cause. Off by default so the headline pps number is
    // the pure receive-path ceiling PAPER 4.11 talks about.
    bool        touch_payload    = false;
};

// ---------------------------------------------------------------- clock

// CLOCK_MONOTONIC microseconds. This is "the client clock" everywhere in
// TRANSPORT.md section 11; nothing in the deadline controller ever mixes it
// with the server clock (PAPER 4.11, clock offset error).
uint64_t now_us();

// ---------------------------------------------------------------- logging

void log_info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_err (const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace nxc
