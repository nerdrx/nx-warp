#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cassert>

#include "basisu_transcoder.h"
#include "basisu_containers_impl.h"
#include "basisu_astc_hdr_core.h"

#define BASISU_ASTC_HELPERS_IMPLEMENTATION
#include "basisu_astc_helpers.h"

using namespace astc_helpers;

static void put_bits(uint32_t out[4], unsigned bit, uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        if ((value >> i) & 1u) out[(bit + i) >> 5] |= 1u << ((bit + i) & 31u);
}

static uint32_t reverse3(uint32_t v) {
    return ((v & 1u) << 2) | (v & 2u) | ((v & 4u) >> 2);
}

static astc_block direct_5x5_cem8(const uint8_t endpoints[6], const uint8_t weights[25]) {
    astc_block out{};
    // Table-82 block mode config 243: 5x5 grid, weight ISE range 5 (8 levels).
    put_bits(out.m_vals, 0, 243, 11);
    put_bits(out.m_vals, 11, 0, 2); // one partition
    put_bits(out.m_vals, 13, CEM_LDR_RGB_DIRECT, 4);
    for (unsigned i = 0; i < 6; ++i) put_bits(out.m_vals, 17 + 6 * i, endpoints[i], 6);
    // ASTC weight sequence is stored in reverse bit order from the high end.
    for (unsigned i = 0; i < 25; ++i) put_bits(out.m_vals, 125 - 3 * i, reverse3(weights[i]), 3);
    return out;
}

static bool check_case(const char* name, unsigned grid, unsigned weight_range,
                       uint32_t expected_mode, const uint8_t endpoints[6],
                       const std::vector<uint8_t>& weights, bool direct_simple) {
    log_astc_block src{};
    src.clear();
    src.m_grid_width = src.m_grid_height = static_cast<uint8_t>(grid);
    src.m_dual_plane = false;
    src.m_weight_ise_range = static_cast<uint8_t>(weight_range);
    src.m_num_partitions = 1;
    src.m_color_endpoint_modes[0] = CEM_LDR_RGB_DIRECT;
    std::copy(endpoints, endpoints + 6, src.m_endpoints);
    std::copy(weights.begin(), weights.end(), src.m_weights);

    const unsigned endpoint_count = 6;
    uint32_t mode = 0;
    if (!get_config_bits(src, mode) || mode != expected_mode) {
        std::fprintf(stderr, "%s: unexpected config %u (expected %u)\n", name, mode, expected_mode);
        return false;
    }
    const int config_bits = 11 + 2 + 4;
    const int weight_bits = get_ise_sequence_bits(static_cast<int>(weights.size()), static_cast<int>(weight_range));
    const int endpoint_space = 128 - config_bits - weight_bits;
    int endpoint_range = -1;
    for (int r = LAST_VALID_ENDPOINT_ISE_RANGE; r >= FIRST_VALID_ENDPOINT_ISE_RANGE; --r) {
        if (get_ise_sequence_bits(endpoint_count, r) <= endpoint_space) { endpoint_range = r; break; }
    }
    if (endpoint_range < 0) return false;
    src.m_endpoint_ise_range = static_cast<uint8_t>(endpoint_range);

    astc_block upstream{};
    int expected_range = -1;
    pack_stats stats{};
    if (!pack_astc_block(upstream, src, &expected_range, &stats)) {
        std::fprintf(stderr, "%s: upstream pack failed (expected range %d)\n", name, expected_range);
        return false;
    }
    log_astc_block decoded{};
    if (!unpack_block(&upstream, decoded, 8, 8) || decoded.m_grid_width != grid ||
        decoded.m_grid_height != grid || decoded.m_num_partitions != 1 ||
        decoded.m_color_endpoint_modes[0] != CEM_LDR_RGB_DIRECT ||
        decoded.m_weight_ise_range != weight_range || decoded.m_endpoint_ise_range != endpoint_range ||
        std::memcmp(decoded.m_endpoints, endpoints, 6) ||
        std::memcmp(decoded.m_weights, weights.data(), weights.size())) {
        std::fprintf(stderr, "%s: upstream unpack roundtrip mismatch\n", name);
        return false;
    }
    if (direct_simple) {
        const astc_block direct = direct_5x5_cem8(endpoints, weights.data());
        if (std::memcmp(&direct, &upstream, sizeof(direct))) {
            std::fprintf(stderr, "%s: direct pack differs from upstream\n", name);
            return false;
        }
    }

    std::array<uint32_t, 64> pixels{};
    if (!decode_block(decoded, pixels.data(), 8, 8, cDecodeModeLDR8)) {
        std::fprintf(stderr, "%s: upstream decode failed\n", name);
        return false;
    }
    std::printf("%s: config=0x%03x endpoint_range=%d endpoint_bits=%d weight_bits=%d bytes=",
        name, mode, endpoint_range, get_ise_sequence_bits(endpoint_count, endpoint_range), weight_bits);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&upstream);
    for (unsigned i = 0; i < 16; ++i) std::printf("%02x", bytes[i]);
    std::printf(" decoded-row-R=");
    for (unsigned x = 0; x < 8; ++x) std::printf("%u%s", (pixels[x] >> 0) & 255u, x == 7 ? "" : ",");
    std::printf("\n");
    return true;
}

int main() {
    init_tables();
    const uint8_t flat_endpoints[6] = { 10, 10, 25, 25, 50, 50 };
    std::vector<uint8_t> flat_weights(25, 0);
    const uint8_t ramp_endpoints[6] = { 0, 63, 0, 63, 0, 63 };
    std::vector<uint8_t> ramp_weights(25);
    for (unsigned y = 0; y < 5; ++y)
        for (unsigned x = 0; x < 5; ++x)
            ramp_weights[y * 5 + x] = static_cast<uint8_t>((7 * x + 2) / 4);
    const uint8_t alt_endpoints[6] = { 0, 79, 7, 71, 15, 63 };
    std::vector<uint8_t> alt_weights(36);
    for (unsigned i = 0; i < alt_weights.size(); ++i) alt_weights[i] = static_cast<uint8_t>(i & 3u);

    bool ok =
        check_case("5x5-flat", 5, 5, 243, flat_endpoints, flat_weights, true) &&
        check_case("5x5-ramp", 5, 5, 243, ramp_endpoints, ramp_weights, true) &&
        check_case("6x6-2bit", 6, 2, 264, alt_endpoints, alt_weights, false);
    // Deterministic packed-bit stress cases, all compared byte-for-byte.
    uint32_t rng = 0x4e585741u;
    for (unsigned n = 0; n < 32 && ok; ++n) {
        uint8_t ep[6];
        std::vector<uint8_t> w(25);
        for (auto& v : ep) { rng = rng * 1664525u + 1013904223u; v = (rng >> 26) & 63u; }
        for (auto& v : w) { rng = rng * 1664525u + 1013904223u; v = (rng >> 29) & 7u; }
        char name[32]; std::snprintf(name, sizeof(name), "5x5-random-%02u", n);
        ok = check_case(name, 5, 5, 243, ep, w, true);
    }
    return ok ? 0 : 1;
}
