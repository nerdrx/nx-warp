// CPU side of the Phase 0 rANS: static tables, the interleaved encoder that
// produces the streams K4 decodes, and a reference decoder used by --selftest.
//
// This is the exact dual of shaders/k4_rans.comp. If the two ever disagree the
// kernel is wrong, not this file: PAPER 3.7 makes the CPU reference normative.
#pragma once
#include <cstdint>
#include <vector>

namespace nxb {

constexpr int kRansLanes   = 8;      // PAPER 6.3: v1 fixes eight lanes
constexpr int kRansCtx     = 8;      // PAPER 3.2.2: eight contexts
constexpr int kRansSymbols = 16;     // 0..14 levels, 15 = escape
constexpr uint32_t kRansL  = 1u << 16;
constexpr int kRansMBits   = 10;
constexpr uint32_t kRansM  = 1u << kRansMBits;

struct RansTables
{
    uint16_t freq[kRansCtx][kRansSymbols];
    uint16_t cum [kRansCtx][kRansSymbols];
    uint8_t  lut [kRansCtx][kRansM];      // slot -> symbol

    void build();
    // Packed exactly as k4_rans.comp expects: 8*256 LUT words (4 bytes each,
    // slot order, little endian) followed by 8*16 words of freq | cum << 16.
    std::vector<uint32_t> packed() const;
};

// One tile's worth of decoded output, for the self test.
struct TileSymbols
{
    // [lane][index] -- index runs 0..symsPerLane-1 in decode order.
    std::vector<int16_t> value[kRansLanes];
};

struct RansStream
{
    std::vector<uint32_t> words;   // the whole bitstream, 4-byte addressed
    std::vector<uint32_t> offsets; // per tile, byte offset of its slot
    std::vector<TileSymbols> expect;  // only filled when keepExpect is set
    size_t payloadBytes = 0;       // total bytes actually used
};

// Builds `tiles` independent tile streams, each carrying kRansLanes lanes of
// `symsPerLane` symbols drawn from the table distributions.
RansStream ransBuildStreams(const RansTables& t, int tiles, int symsPerLane,
                            uint64_t seed, bool keepExpect);

// Reference decoder: mirrors k4_rans.comp step for step, including the
// ballot-derived read order (simulated here as a plain lane loop).
bool ransDecodeCheck(const RansTables& t, const RansStream& s,
                     int tiles, int symsPerLane, int* firstBadTile);

// Context update rule, shared by encoder and decoder so it cannot drift.
inline int ransNextCtx(int k, int prevLevel)
{
    int band = (k < 1) ? 0 : ((k < 4) ? 1 : ((k < 10) ? 2 : 3));
    return band + (prevLevel != 0 ? 4 : 0);
}

} // namespace nxb
