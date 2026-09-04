// A 5x7 bitmap font and the character grid the HUD shader reads.
//
// Deliberately tiny: 54 glyphs covering uppercase, digits and the punctuation
// the HUD actually prints. Seven u32 per glyph (five significant bits per row,
// bit 4 leftmost) is 1512 bytes of SSBO -- cheaper to upload once than to pack,
// and it keeps the shader's inner loop to a single indexed load.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace nxc {

// Glyph order. Index 0 is space; lookup is a linear scan done once per character
// on the CPU, at HUD build time, never in the shader.
extern const char* const kCharset;

// 7 * glyph_count words, ready to memcpy into the font SSBO.
const std::vector<uint32_t>& font_rows();
uint32_t font_glyph_count();

// HUD colour indices, matching hud_colour() in hud.comp.
enum HudColour : uint8_t {
    kColBody = 0, kColHeading = 1, kColGood = 2, kColWarn = 3, kColBad = 4, kColDim = 5,
};

// A fixed character grid. The HUD is rebuilt from scratch every frame; at
// 96x30 cells that is 2880 words, which is nothing next to a frame.
class TextCanvas {
public:
    TextCanvas(uint32_t cols, uint32_t rows);

    void clear();
    void put(uint32_t col, uint32_t row, char ch, uint8_t colour = kColBody);
    void text(uint32_t col, uint32_t row, const char* s, uint8_t colour = kColBody);
    void printf_at(uint32_t col, uint32_t row, uint8_t colour, const char* fmt, ...)
        __attribute__((format(printf, 5, 6)));

    uint32_t cols() const { return cols_; }
    uint32_t rows() const { return rows_; }
    const std::vector<uint32_t>& cells() const { return cells_; }
    size_t bytes() const { return cells_.size() * sizeof(uint32_t); }

private:
    uint32_t cols_, rows_;
    std::vector<uint32_t> cells_;
};

}  // namespace nxc
