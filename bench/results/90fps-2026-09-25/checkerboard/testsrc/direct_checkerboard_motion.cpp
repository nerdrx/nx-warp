// CPU reconstruction visual fixture for synthetic direct NXDF frames.
#include "nxwarp_direct.h"
#include "nxwarp_direct_checkerboard.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace wivrn::nxwarp_direct;
using Bytes = std::vector<uint8_t>;

static Bytes read_file(const std::string & path)
{
	std::ifstream f(path, std::ios::binary);
	return Bytes(std::istreambuf_iterator<char>(f), {});
}

static uint32_t rgb565(uint32_t v)
{
	const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
	return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

static uint32_t mix_palette(uint32_t endpoints, uint32_t selector)
{
	const uint32_t a = rgb565(endpoints & 65535), b = rgb565(endpoints >> 16);
	uint32_t c = 0;
	for (unsigned shift: {16u, 8u, 0u})
		c |= ((((3 - selector) * ((a >> shift) & 255) + selector * ((b >> shift) & 255) + 1) / 3) & 255) << shift;
	return c;
}

static uint32_t descriptor(layout l, const frame_view & v, uint32_t x, uint32_t y)
{
	const uint32_t cols = l.width / 32, tile = (y / 32) * (cols * l.eyes) + x / 32;
	return read32(v.descriptors, size_t(tile) * 4);
}

static bool sample_present(bool checker, uint32_t phase, uint32_t x, uint32_t y, uint32_t shift)
{
	return !checker || ((((x >> shift) ^ (y >> shift)) & 1u) == phase);
}

static uint32_t decode(layout l, std::span<const uint8_t> frame, const frame_view & parsed,
                       uint32_t x, uint32_t y, bool nearest_missing)
{
	const bool checker = checker_frame(frame);
	const uint32_t phase = checker_phase(frame);
	uint32_t d = descriptor(l, parsed, x, y), mode = d >> 30;
	if (mode == 3) return d & 0xffffffu;
	const bool native = d & 0x20000000u, packed = d & 0x10000000u;
	const uint32_t shift = native ? 0 : mode;
	uint32_t px = x % 32, py = y % 32;
	if (nearest_missing && !sample_present(checker, phase, x, y, shift))
		px = native ? (px < 31 ? px + 1 : px - 1) : (px >> shift) % 8 < 7 ? px + (1u << shift) : px - (1u << shift);
	const uint32_t offset = d & (native ? 0x0fffffffu : 0x3fffffffu);
	if (native) {
		uint32_t index = py * 32 + px;
		if (checker) {
			const uint32_t first_x = (checker_phase(frame)) ^ (py & 1u);
			index = py * 16 + (px - first_x) / 2;
		}
		uint32_t pixel;
		if (packed) {
			const uint32_t pair = read32(parsed.blocks, 4ull * (offset + index / 2));
			pixel = rgb565((pair >> (16 * (index & 1))) & 65535u);
		} else {
			pixel = read32(parsed.blocks, 4ull * (offset + index)) & 0xffffffu;
		}
		return pixel;
	}
	const uint32_t qx = px >> shift, qy = py >> shift;
	const uint32_t bpr = 4 >> shift;
	const uint32_t stride = checker ? 3 : 5;
	const uint32_t block = offset + stride * ((qy / 8) * bpr + qx / 8);
	const uint32_t endpoints = read32(parsed.blocks, 4ull * block);
	uint32_t selector_index = (qy % 8) * 8 + qx % 8;
	if (checker) {
		const uint32_t lx = qx % 8, ly = qy % 8;
		const uint32_t first_x = (checker_phase(frame)) ^ (ly & 1u);
		selector_index = ly * 4 + (lx - first_x) / 2;
	}
	const uint32_t packed_selectors = read32(parsed.blocks, 4ull * (block + 1 + selector_index / 16));
	const uint32_t selector = (packed_selectors >> (2 * (selector_index % 16))) & 3;
	return mix_palette(endpoints, selector);
}

static uint32_t sample(layout l, std::span<const uint8_t> current, const frame_view & cv,
                       std::span<const uint8_t> previous, const frame_view * pv,
                       uint32_t x, uint32_t y, bool accept_previous)
{
	const bool current_checker = checker_frame(current);
	const uint32_t d = descriptor(l, cv, x, y), mode = d >> 30;
	const uint32_t shift = (d & 0x20000000u) ? 0 : mode;
	if (mode == 3 || sample_present(current_checker, checker_phase(current), x, y, shift)) return decode(l, current, cv, x, y, false);
	if (accept_previous && pv) {
		const uint32_t old = descriptor(l, *pv, x, y);
		if ((d >> 30) == (old >> 30) && ((d ^ old) & 0x30000000u) == 0 &&
		    sample_present(checker_frame(previous), checker_phase(previous), x, y, shift))
			return decode(l, previous, *pv, x, y, false);
	}
	return decode(l, current, cv, x, y, true);
}

static void write_ppm(const std::string & path, layout l, std::span<const uint8_t> raw, const frame_view & raw_view,
                      std::span<const uint8_t> checker, const frame_view & checker_view,
                      std::span<const uint8_t> old_checker, const frame_view * old_view, bool accept_previous)
{
	constexpr uint32_t x0 = 880, y0 = 930, width = 448, height = 252;
	std::ofstream out(path, std::ios::binary);
	out << "P6\n" << width * 4 << " " << height << "\n255\n";
	for (uint32_t y = 0; y < height; ++y)
		for (unsigned pane = 0; pane < 4; ++pane)
			for (uint32_t x = 0; x < width; ++x) {
				const uint32_t px = x0 + x, py = y0 + y;
				uint32_t c;
				if (pane == 0) c = decode(l, raw, raw_view, px, py, false);
				else if (pane == 1) c = sample(l, checker, checker_view, old_checker, old_view, px, py, accept_previous);
				else if (pane == 2) c = sample(l, checker, checker_view, {}, nullptr, px, py, false);
				else {
					const uint32_t d = descriptor(l, checker_view, px, py);
					const uint32_t shift = (d & 0x20000000u) ? 0u : d >> 30;
					if ((d >> 30) == 3 || sample_present(true, checker_phase(checker), px, py, shift))
						c = decode(l, checker, checker_view, px, py, false);
					else c = (((px >> shift) + (py >> shift)) & 1u) ? 0x28232fu : 0x12151bu;
				}
				const char rgb[3] = {char(c >> 16), char(c >> 8), char(c)};
				out.write(rgb, 3);
			}
}

int main(int argc, char ** argv)
{
	if (argc < 4) {
		std::fprintf(stderr, "usage: %s output-prefix current.nxdf [previous.nxdf ...]\n", argv[0]);
		return 2;
	}
	layout l{2176, 2176, 2, true, 256, false, false, false, true};
	for (int i = 2; i < argc; ++i) {
		const Bytes current = read_file(argv[i]);
		const Bytes previous_raw = i > 2 ? read_file(argv[i - 1]) : Bytes{};
		if (!parse_frame(l, current) || (i > 2 && !parse_frame(l, previous_raw))) return 3;
		Bytes checker_storage, previous_storage;
		const uint32_t phase = uint32_t(i - 2) & 1u;
		const auto checker = checkerboard_frame(l, current, checker_storage, phase);
		const auto old_checker = i > 2 ? checkerboard_frame(l, previous_raw, previous_storage, phase ^ 1u) : std::span<const uint8_t>{};
		const auto cv = parse_frame(l, checker);
		const auto pv = i > 2 ? parse_frame(l, old_checker) : std::optional<frame_view>{};
		const auto raw_view = parse_frame(l, current);
		if (checker.empty() || !cv || !raw_view || (i > 2 && !pv)) return 4;
		const std::string name = std::string(argv[1]) + std::to_string(i - 2) + ".ppm";
		write_ppm(name, l, current, *raw_view, checker, *cv, old_checker, pv ? &*pv : nullptr, i > 2);
	}
}
