#include "nxwarp_direct_lz4.h"
#include "nxwarp_direct_native.h"
#include "nxwarp_direct_zstd.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wivrn::nxwarp_direct;
using bytes = std::vector<uint8_t>;
using clock_type = std::chrono::steady_clock;
constexpr uint32_t width = 2176, height = 2176, eyes = 2, side = 256;
constexpr uint32_t tile_side = 32, native_flag = 1u << 29;

static bytes read_file(const fs::path & path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) throw std::runtime_error("cannot open " + path.string());
	return {std::istreambuf_iterator<char>(in), {}};
}

static void write_file(const fs::path & path, std::span<const uint8_t> data)
{
	std::ofstream out(path, std::ios::binary);
	if (!out || !out.write(reinterpret_cast<const char *>(data.data()), data.size()))
		throw std::runtime_error("cannot write " + path.string());
}

static bool decode(layout l, std::span<const uint8_t> wire, bytes & out)
{
	if (is_zstd(wire)) return decompress_zstd(l, wire, out);
	if (is_lz4(wire)) return decompress_lz4(l, wire, out);
	out.assign(wire.begin(), wire.end());
	return bool(parse_frame(l, out));
}

struct selection
{
	bytes wire;
	size_t incumbent_bytes = 0, row_bytes = 0;
	bool row_selected = false;
};

static selection production_select(layout l, std::span<const uint8_t> raw)
{
	bytes lz4, dense, predicted, scratch, row;
	const auto fast = compress_lz4(raw, lz4);
	auto zstd = compress_zstd(raw, dense);
	const auto zstd_predicted = compress_zstd_predicted(raw, predicted, scratch);
	if (zstd_predicted.size() * 100 <= zstd.size() * 95) zstd = zstd_predicted;
	auto incumbent = zstd.size() * 100 <= fast.size() * 90 ? zstd : fast;
	selection result;
	result.incumbent_bytes = incumbent.size();
	const auto row_predicted = compress_zstd_row_predicted(l, raw, row, scratch);
	result.row_bytes = row_predicted.size();
	result.row_selected = is_zstd(row_predicted) && read32(row_predicted, 4) == 3 &&
	                      row_predicted.size() * 100 <= incumbent.size() * 95;
	if (result.row_selected) incumbent = row_predicted;
	result.wire.assign(incumbent.begin(), incumbent.end());
	return result;
}

static uint32_t pixel(const frame_view & frame, uint32_t descriptor,
                      uint32_t x, uint32_t y)
{
	if (descriptor & native_flag)
	{
		const uint32_t off = descriptor & 0x0fffffffu;
		return read32(frame.blocks, size_t(off + y * tile_side + x) * 4) & 0xffffffu;
	}
	return native_base_pixel(frame.blocks, descriptor, x, y);
}

static bytes centre_only(layout l, std::span<const uint8_t> source,
                         size_t & retained_tiles, size_t & core_pixels,
                         size_t & active_pixels)
{
	const auto parsed = parse_frame(l, source);
	if (!parsed) throw std::runtime_error("invalid source NXDF");
	const uint32_t tx_count = width / tile_side, ty_count = height / tile_side;
	const double cx = width / 2.0, cy = height / 2.0, radius = 384.0;
	std::vector<uint32_t> descriptors(l.tile_count());
	std::vector<uint8_t> blocks;
	for (uint32_t ty = 0; ty < ty_count; ++ty)
	for (uint32_t eye = 0; eye < eyes; ++eye)
	for (uint32_t tx = 0; tx < tx_count; ++tx)
	{
		const size_t tile = size_t(ty) * tx_count * eyes + eye * tx_count + tx;
		const uint32_t old = read32(parsed->descriptors, tile * 4);
		const double x0 = double(tx * tile_side), x1 = x0 + tile_side;
		const double y0 = double(ty * tile_side), y1 = y0 + tile_side;
		const double dx = std::max({x0 - cx, cx - x1, 0.0});
		const double dy = std::max({y0 - cy, cy - y1, 0.0});
		if (dx * dx + dy * dy > radius * radius)
		{
			descriptors[tile] = 0xc0000000u;
			continue;
		}
		++retained_tiles;
		const uint32_t mode = old >> 30;
		if (mode == 3 && !(old & native_flag))
		{
			descriptors[tile] = old;
			continue;
		}
		const uint32_t words = (old & native_flag) ? native_rgb_words : (80u >> (2 * mode));
		while ((blocks.size() / 4) % 5) append32(blocks, 0);
		const uint32_t offset = uint32_t(blocks.size() / 4);
		const uint32_t mask = (old & native_flag) ? 0x30000000u : 0xc0000000u;
		descriptors[tile] = (old & mask) | offset;
		const uint32_t old_offset = old & 0x0fffffffu;
		const size_t begin = size_t(old_offset) * 4, count = size_t(words) * 4;
		if (begin + count > parsed->blocks.size()) throw std::runtime_error("source tile range overflow");
		blocks.insert(blocks.end(), parsed->blocks.begin() + begin, parsed->blocks.begin() + begin + count);
	}
	bytes out;
	for (uint32_t v: {frame_magic, native_version, l.tile_count(), uint32_t(blocks.size() / 4)}) append32(out, v);
	for (uint32_t d: descriptors) append32(out, d);
	out.insert(out.end(), blocks.begin(), blocks.end());
	if (!parse_frame(l, out)) throw std::runtime_error("rebuilt centre NXDF failed parse");
	const auto rebuilt = parse_frame(l, out);
	for (uint32_t eye = 0; eye < eyes; ++eye)
	for (uint32_t y = 0; y < height; ++y)
	for (uint32_t x = 0; x < width; ++x)
	{
		const double dx = double(x) + 0.5 - cx, dy = double(y) + 0.5 - cy;
		const double distance2 = dx * dx + dy * dy;
		if (distance2 > 384.0 * 384.0) continue;
		const size_t tile = size_t(y / 32) * tx_count * eyes + eye * tx_count + x / 32;
		const uint32_t a = read32(parsed->descriptors, tile * 4);
		const uint32_t b = read32(rebuilt->descriptors, tile * 4);
		if (pixel(*parsed, a, x % 32, y % 32) != pixel(*rebuilt, b, x % 32, y % 32))
			throw std::runtime_error("restored r384 NX-decoded active region differs");
		++active_pixels;
		if (distance2 <= 128.0 * 128.0) ++core_pixels;
	}
	return out;
}

int main(int argc, char ** argv)
try
{
	if (argc != 4)
	{
		std::cerr << "usage: " << argv[0] << " CORPUS OUTPUT_DIR SUMMARY_JSON\n";
		return 2;
	}
	const fs::path corpus = argv[1], output = argv[2], summary_path = argv[3];
	fs::create_directories(output);
	layout l{width, height, eyes, true, side, false, true, true, false, false};
	l.native_row_predictor = true;
	const char * names[] = {"forest-s0", "forest-s8", "forest-s16", "dark-s0", "dark-s8", "dark-s16"};
	struct entry { std::string name; bytes raw, wire; size_t retained, core_pixels, active_pixels; selection selected; };
	std::vector<entry> entries;
	for (const char * name: names)
	{
		entry e{};
		e.name = name;
		const auto source = read_file(corpus / (e.name + ".nxdf"));
		e.raw = centre_only(l, source, e.retained, e.core_pixels, e.active_pixels);
		e.selected = production_select(l, e.raw);
		bytes restored;
		if (!decode(l, e.selected.wire, restored) || restored != e.raw)
			throw std::runtime_error("selected detail failed exact decode: " + e.name);
		e.wire = e.selected.wire;
		write_file(output / (e.name + ".detail.bin"), e.wire);
		write_file(output / (e.name + ".nxdf"), e.raw);
		entries.push_back(std::move(e));
	}
	std::ofstream samples(output / "samples.csv");
	samples << "fixture,phase,iteration,decode_us,selected_bytes,raw_bytes,exact\n";
	std::ofstream summary(summary_path);
	summary << "{\n  \"scope\": \"center-only sparse NXDF, selector envelope, CPU helper decode only\",\n"
	        << "  \"method\": {\"preserved_tile_disk_radius_px\": 384, "
	        << "\"map\": \"9248 row-interleaved stereo descriptors\", "
	        << "\"exterior\": \"inline black mode-3 descriptors\", "
	        << "\"selector\": \"production LZ4 vs dense/predicted Zstd, then native-row v3 5% gate\", "
	        << "\"validation\": \"decoded RGB equality to original NXDF within r384; r128 core counted separately\", "
	        << "\"timing\": \"CPU helper decode only\"},\n"
	        << "  \"preserved_tile_disk_radius_px\": 384, \"validated_nx_decoded_active_radius_px\": 384,\n"
	        << "  \"validated_nx_decoded_core_radius_px\": 128,\n"
	        << "  \"warmups\": 6, \"timed_iterations\": 24,\n  \"fixtures\": {\n";
	for (size_t fi = 0; fi < entries.size(); ++fi)
	{
		auto & e = entries[fi];
		std::vector<double> times;
		for (int i = 0; i < 30; ++i)
		{
			bytes restored;
			const auto start = clock_type::now();
			const bool ok = decode(l, e.wire, restored);
			const double us = std::chrono::duration<double, std::micro>(clock_type::now() - start).count();
			if (!ok || restored != e.raw) throw std::runtime_error("timed exact decode mismatch");
			samples << e.name << ',' << (i < 6 ? "warm" : "timed") << ',' << (i < 6 ? i : i - 6)
			        << ',' << us << ',' << e.wire.size() << ',' << e.raw.size() << ",1\n";
			if (i >= 6) times.push_back(us);
		}
		auto sorted = times;
		std::sort(sorted.begin(), sorted.end());
		auto percentile = [&](double q) { return sorted[size_t(std::ceil(sorted.size() * q)) - 1]; };
		summary << "    \"" << e.name << "\": {\"selected_bytes\": " << e.wire.size()
		        << ", \"raw_bytes\": " << e.raw.size()
		        << ", \"retained_tiles_per_frame\": " << e.retained
		        << ", \"validated_core_pixels\": " << e.core_pixels
		        << ", \"validated_active_pixels\": " << e.active_pixels
		        << ", \"decode_p50_us\": " << percentile(.5)
		        << ", \"decode_p95_us\": " << percentile(.95) << "}"
		        << (fi + 1 == entries.size() ? "\n" : ",\n");
	}
	summary << "  }\n}\n";
	return 0;
}
catch (const std::exception & e)
{
	std::cerr << e.what() << '\n';
	return 1;
}
