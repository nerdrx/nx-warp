#include "nxwarp_direct.h"
#include "nxwarp_direct_checkerboard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace wivrn::nxwarp_direct;

static uint32_t word(std::span<const uint8_t> bytes, size_t i)
{
	return read32(bytes, i * 4);
}

static std::vector<uint8_t> palette_frame()
{
	std::vector<uint8_t> bytes = frame_header(1, 5);
	append32(bytes, 0x80000000u); // mode 2, block offset 0
	append32(bytes, 0x12345678); // shared palette endpoint pair
	for (uint32_t i = 0; i < 4; ++i) {
		uint32_t selectors = 0;
		for (uint32_t j = 0; j < 16; ++j)
			selectors |= ((i * 16 + j) * 3 + 1) % 4 << (2 * j);
		append32(bytes, selectors);
	}
	return bytes;
}

static std::vector<uint8_t> native_frame(bool packed)
{
	layout l{256, 256, 2, true, 256, packed};
	const uint32_t words = packed ? 515 : native_rgb_words;
	std::vector<uint8_t> bytes = frame_header(l.tile_count(), words, native_version);
	append32(bytes, 0x20000000u | (packed ? 0x10000000u : 0u));
	for (uint32_t i = 1; i < l.tile_count(); ++i) append32(bytes, 0xc0123456u);
	for (uint32_t i = 0; i < words; ++i)
		append32(bytes, packed ? ((i * 31u & 65535u) | ((i * 73u & 65535u) << 16)) : i * 0x010203u);
	return bytes;
}

int main()
{
	// Palette stays shared. Each phase carries 32 original selectors in row order.
	layout palette_layout{32, 32, 1};
	const auto old_palette = palette_frame();
	assert(parse_frame(palette_layout, old_palette));
	std::vector<uint8_t> phases[2];
	for (uint32_t phase = 0; phase != 2; ++phase) {
		const auto compact = checkerboard_frame(layout{32, 32, 1, false, 256, true, false, false, true}, old_palette, phases[phase], phase);
		assert(!compact.empty() && checker_frame(compact) && checker_phase(compact) == phase);
		assert(parse_frame(layout{32, 32, 1, false, 256, true, false, false, true}, compact));
		assert(read32(compact, 12) == 3);
		assert(word(compact, 5) == 0x12345678u); // block endpoint is retained verbatim
	}
	for (uint32_t phase = 0; phase != 2; ++phase) {
		const auto compact = std::span<const uint8_t>(phases[phase]);
		for (uint32_t y = 0; y < 8; ++y)
			for (uint32_t x = 0; x < 8; ++x)
				if (((x + y) & 1) == phase) {
					const uint32_t index = y * 8 + x;
					const uint32_t expected = (index * 3 + 1) % 4;
					const uint32_t compact_index = y * 4 + x / 2;
					const uint32_t got = (word(compact, 6 + compact_index / 16) >> (2 * (compact_index % 16))) & 3;
					assert(got == expected);
				}
	}

	// Coarse modes 0 and 1 each hold multiple independent palette blocks;
	// checker compaction must retain endpoints and remap each selector lattice.
	for (uint32_t mode: {0u, 1u}) {
		const uint32_t block_count = 16u >> (2 * mode), old_words = block_count * 5;
		std::vector<uint8_t> raw = frame_header(1, old_words);
		append32(raw, mode << 30);
		for (uint32_t block = 0; block < block_count; ++block) {
			append32(raw, 0x10203040u + block);
			for (uint32_t lane = 0; lane < 4; ++lane) {
				uint32_t selectors = 0;
				for (uint32_t bit = 0; bit < 16; ++bit) {
					const uint32_t pixel = lane * 16 + bit;
					selectors |= ((pixel * 3 + block) & 3u) << (2 * bit);
				}
				append32(raw, selectors);
			}
		}
		assert(parse_frame(palette_layout, raw));
		for (uint32_t phase = 0; phase != 2; ++phase) {
			std::vector<uint8_t> compact_storage;
			const auto compact = checkerboard_frame(layout{32, 32, 1, false, 256, true, false, false, true}, raw, compact_storage, phase);
			const uint32_t compact_words = block_count * 3;
			assert(!compact.empty() && read32(compact, 12) == compact_words);
			assert(parse_frame(layout{32, 32, 1, false, 256, true, false, false, true}, compact));
			for (uint32_t block = 0; block < block_count; ++block) {
				const uint32_t old_base = 1 + block * 5, new_base = 5 + block * 3;
				assert(word(compact, new_base) == word(raw, 4 + old_base));
				for (uint32_t y = 0; y < 8; ++y)
					for (uint32_t x = 0; x < 8; ++x)
						if (((x + y) & 1u) == phase) {
							const uint32_t src_i = y * 8 + x;
							const uint32_t src = (word(raw, 4 + old_base + 1 + src_i / 16) >> (2 * (src_i % 16))) & 3;
							const uint32_t dst_i = y * 4 + x / 2;
							const uint32_t dst = (word(compact, new_base + 1 + dst_i / 16) >> (2 * (dst_i % 16))) & 3;
							assert(dst == src);
						}
			}
		}
	}

	// Both native formats preserve exactly the source RGB values selected by phase.
	for (bool packed: {false, true}) {
		layout native{256, 256, 2, true, 256, packed, false, false, true};
		const auto raw = native_frame(packed);
		assert(parse_frame(native, raw));
		for (uint32_t phase = 0; phase != 2; ++phase) {
			std::vector<uint8_t> out;
			const auto compact = checkerboard_frame(native, raw, out, phase);
			assert(!compact.empty() && parse_frame(native, compact));
			assert(read32(compact, 12) == (packed ? 256u : 512u));
			uint32_t sample = 0;
			for (uint32_t y = 0; y < 32; ++y)
				for (uint32_t x = 0; x < 32; ++x)
					if (((x + y) & 1) == phase) {
						const uint32_t pixel = y * 32 + x;
						const uint32_t expected = packed
							? ((read32(raw, (16 + native.tile_count() * 4) + (pixel / 2) * 4) >> (16 * (pixel % 2))) & 65535u)
							: read32(raw, (16 + native.tile_count() * 4) + pixel * 4);
						const uint32_t actual = packed
							? ((word(compact, 4 + native.tile_count() + sample / 2) >> (16 * (sample % 2))) & 65535u)
							: word(compact, 4 + native.tile_count() + sample);
						assert(actual == expected);
						++sample;
					}
			assert(sample == 512);
		}
	}

	// Stream versions 1..20 remain accepted; +32 advertises checker support.
	for (uint32_t version_number = 1; version_number <= 20; ++version_number) {
		const bool native = version_number >= 7;
		const uint32_t side = version_number >= 9 ? 256u : 128u;
		const bool packed = (version_number >= 11 && version_number <= 12) ||
		                    (version_number >= 15 && version_number <= 16) || version_number >= 19;
		const bool zstd = version_number >= 13;
		const bool predictor = version_number >= 17;
		layout l{native ? 512u : 32u, native ? 512u : 32u, native ? 2u : 1u,
		         native, side, packed, zstd, predictor, false};
		const bool trusted = (version_number & 1u) == 0;
		const auto header = stream_header(l, trusted, version_number >= 3, version_number >= 5);
		assert(read32(header, 4) == version_number && parse_stream(header));
		l.checkerboard = true;
		const auto checker_header = stream_header(l, trusted, version_number >= 3, version_number >= 5);
		assert(read32(checker_header, 4) == version_number + 32 && parse_stream(checker_header));
		assert(parse_stream(checker_header)->checkerboard && !parse_stream(header)->checkerboard);
	}

	layout checker{32, 32, 1, false, 256, true, false, false, true};
	const auto old = palette_frame();
	assert(parse_frame(checker, old));
	for (size_t n = 0; n < old.size(); ++n) assert(!parse_frame(checker, std::span(old).first(n)));
	assert(!checkerboard_frame(checker, old, phases[0], 2).size());
	assert(!parse_frame(palette_layout, phases[0])); // old stream/layout must reject checker frames
	std::vector<uint8_t> bad = phases[0];
	bad[16] |= 1; // block offset moved to a truncated block
	assert(!parse_frame(checker, bad));
	bad = old;
	bad[4] = 1; // unsupported frame flags are rejected
	bad[5] = 4; // reserved flag bit 0x400
	assert(!parse_frame(checker, bad));
	std::puts("direct checkerboard: ok");
}
