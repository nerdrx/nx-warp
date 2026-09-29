#include "nxwarp_direct_native.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wivrn::nxwarp_direct;

constexpr uint32_t width = 2176, height = 2176, eyes = 2, side = 256;
constexpr uint32_t native_flag = 1u << 29, packed_flag = 1u << 28;

static std::vector<uint8_t> read_file(const fs::path & path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) throw std::runtime_error("cannot open " + path.string());
	return {std::istreambuf_iterator<char>(f), {}};
}

static void write_ppm(const fs::path & path, const std::vector<uint8_t> & rgb)
{
	std::ofstream f(path, std::ios::binary);
	if (!f) throw std::runtime_error("cannot create " + path.string());
	f << "P6\n" << width * eyes << ' ' << height << "\n255\n";
	f.write(reinterpret_cast<const char *>(rgb.data()), rgb.size());
	if (!f) throw std::runtime_error("failed writing " + path.string());
}

static void put_rgb(uint8_t * p, uint32_t rgb)
{
	p[0] = uint8_t(rgb >> 16);
	p[1] = uint8_t(rgb >> 8);
	p[2] = uint8_t(rgb);
}

static void export_frame(const fs::path & input, const fs::path & output)
{
	const auto bytes = read_file(input);
	const layout l{width, height, eyes, true, side, false, false, false};
	const auto frame = parse_frame(l, bytes);
	if (!frame) throw std::runtime_error("invalid raw NXDF frame: " + input.string());
	const uint32_t tiles_x = width / 32, tiles_y = height / 32;
	const uint32_t ox = ((width - side) / 2) & ~31u;
	const uint32_t oy = ((height - side) / 2) & ~31u;
	std::vector<uint8_t> rgb(size_t(width) * eyes * height * 3);
	std::array<uint32_t, 4> modes{};
	std::set<uint32_t> periphery_colours;
	std::set<uint32_t> flat_tile_colours;
	uint64_t periphery_pixels = 0;
	uint32_t native_tiles = 0;

	for (uint32_t eye = 0; eye < eyes; ++eye)
	for (uint32_t ty = 0; ty < tiles_y; ++ty)
	for (uint32_t tx = 0; tx < tiles_x; ++tx)
	{
		// NXDF stores each scanline as left-eye tiles, then right-eye tiles.
		const uint32_t d = read32(frame->descriptors, (ty * tiles_x * eyes + eye * tiles_x + tx) * 4);
		const uint32_t mode = d >> 30;
		++modes[mode];
		if (mode == 3 && !(d & native_flag))
			flat_tile_colours.insert(d & 0xffffffu);
		const bool native = (d & native_flag) != 0;
		if (native)
		{
			if (mode != 0 || (d & packed_flag))
				throw std::runtime_error("expected RGB888 native tiles");
			if (tx * 32 < ox || tx * 32 >= ox + side || ty * 32 < oy || ty * 32 >= oy + side)
				throw std::runtime_error("native descriptor outside expected 256x256 centre");
			++native_tiles;
		}
		for (uint32_t y = 0; y < 32; ++y)
		for (uint32_t x = 0; x < 32; ++x)
		{
			uint32_t colour;
			if (native)
			{
				const uint32_t off = d & 0x0fffffffu;
				const uint32_t i = y * 32 + x;
				colour = read32(frame->blocks, size_t(off + i) * 4) & 0xffffffu;
			}
			else
				colour = native_base_pixel(frame->blocks, d, x, y);

			const uint32_t px = tx * 32 + x, py = ty * 32 + y;
			const size_t at = (size_t(py) * (width * eyes) + eye * width + px) * 3;
			put_rgb(rgb.data() + at, colour);
			if (!(px >= ox && px < ox + side && py >= oy && py < oy + side))
			{
				++periphery_pixels;
				periphery_colours.insert(colour);
			}
		}
	}
	for (uint32_t y = 0; y < height; ++y)
	{
		const uint8_t * row = rgb.data() + size_t(y) * width * eyes * 3;
		if (!std::equal(row, row + width * 3, row + width * 3))
			throw std::runtime_error("duplicated-eye pixels differ");
	}
	write_ppm(output, rgb);
	std::cout << input.filename().string() << ": P6 " << width * eyes << 'x' << height
	          << ", native_tiles=" << native_tiles << ", modes=[" << modes[0] << ',' << modes[1]
	          << ',' << modes[2] << ',' << modes[3] << "], periphery_pixels=" << periphery_pixels
	          << ", unique_periphery_RGB=" << periphery_colours.size()
	          << ", unique_inline_flat_tile_RGB=" << flat_tile_colours.size() << "\n";
}

int main(int argc, char ** argv)
try
{
	if (argc != 3)
	{
		std::cerr << "usage: " << argv[0] << " CORPUS_DIR OUTPUT_DIR\n";
		return 2;
	}
	const fs::path corpus = argv[1], output = argv[2];
	fs::create_directories(output);
	for (const char * name: {"forest-s0.nxdf", "forest-s8.nxdf", "forest-s16.nxdf",
	                         "dark-s0.nxdf", "dark-s8.nxdf", "dark-s16.nxdf"})
		export_frame(corpus / name, output / (fs::path(name).stem().string() + ".ppm"));
	return 0;
}
catch (const std::exception & e)
{
	std::cerr << "export failed: " << e.what() << '\n';
	return 1;
}
