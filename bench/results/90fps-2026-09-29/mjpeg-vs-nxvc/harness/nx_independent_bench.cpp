#include "nxwarp_direct_lz4.h"
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

struct fixture
{
	std::string name;
	bytes raw, selected, safety;
	size_t incumbent_bytes = 0, row_bytes = 0;
	size_t safety_raw_bytes = 0;
	bool row_selected = false;
};

static bytes select_independent(layout l, std::span<const uint8_t> raw, fixture & f)
{
	bytes lz4, dense, predicted, predictor_scratch, row;
	const auto fast = compress_lz4(raw, lz4); // production lz4_hc=false selector
	auto zstd = compress_zstd(raw, dense);
	const auto zstd_predicted = compress_zstd_predicted(raw, predicted, predictor_scratch);
	if (zstd_predicted.size() * 100 <= zstd.size() * 95) zstd = zstd_predicted;
	auto incumbent = zstd.size() * 100 <= fast.size() * 90 ? zstd : fast;
	const auto row_predicted = compress_zstd_row_predicted(l, raw, row, predictor_scratch);
	f.incumbent_bytes = incumbent.size();
	f.row_bytes = row_predicted.size();
	bytes restored;
	if (is_zstd(row_predicted) && read32(row_predicted, 4) == 3 &&
	    (!decode(l, row_predicted, restored) || restored.size() != raw.size() ||
	     !std::equal(restored.begin(), restored.end(), raw.begin())))
		throw std::runtime_error("native-row decode mismatch: " + f.name);
	f.row_selected = is_zstd(row_predicted) && read32(row_predicted, 4) == 3 &&
	                  row_predicted.size() * 100 <= incumbent.size() * 95;
	if (f.row_selected)
		incumbent = row_predicted;
	return {incumbent.begin(), incumbent.end()};
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
	layout l{2176, 2176, 2, true, 256, false, true, true, false, false};
	l.native_row_predictor = true;
	const char * names[] = {"forest-s0", "forest-s8", "forest-s16", "dark-s0", "dark-s8", "dark-s16"};
	std::vector<fixture> fixtures;
	for (const char * name : names)
	{
		fixture f;
		f.name = name;
		f.raw = read_file(corpus / (f.name + ".nxdf"));
		if (!parse_frame(l, f.raw)) throw std::runtime_error("invalid raw NXDF: " + f.name);
		f.selected = select_independent(l, f.raw, f);
		bytes restored;
		if (!decode(l, f.selected, restored) || restored != f.raw)
			throw std::runtime_error("selected envelope failed exact decode: " + f.name);
		write_file(output / (f.name + ".detail.bin"), f.selected);
		// 578 tiles means 17x17 tiles per eye: 544x544. Production rounds
		// source/4 up to a 32-pixel boundary for this safety stream.
		const layout safety_layout{544, 544, 2, false, 256, true, false, false, false, false};
		const bytes safety_raw = read_file(corpus / (f.name + ".nxdf.safety.nxdf"));
		if (!parse_frame(safety_layout, safety_raw))
			throw std::runtime_error("invalid raw safety NXDF: " + f.name);
		f.safety_raw_bytes = safety_raw.size();
		bytes safety_compressed;
		const auto safety_wire = compress_lz4(safety_raw, safety_compressed);
		f.safety.assign(safety_wire.begin(), safety_wire.end());
		bytes safety_restored;
		const bool safety_ok = is_lz4(f.safety)
			? decompress_lz4(safety_layout, f.safety, safety_restored)
			: (safety_restored = f.safety, bool(parse_frame(safety_layout, safety_restored)));
		if (!safety_ok || safety_restored != safety_raw)
			throw std::runtime_error("safety LZ4 exact decode failed: " + f.name);
		write_file(output / (f.name + ".safety.bin"), f.safety);
		fixtures.push_back(std::move(f));
	}
	std::ofstream samples(output / "samples.csv");
	samples << "fixture,phase,iteration,decode_us,selected_bytes,raw_bytes,exact\n";
	std::ofstream summary(summary_path);
	summary << "{\n  \"scope\": \"independent selected detail envelope; CPU helper decode only\",\n"
	        << "  \"layout\": \"2176x2176 per eye, stereo, RGB888 native 256, Zstd predictor\",\n"
	        << "  \"warmups\": 6, \"timed_iterations\": 24,\n"
	        << "  \"fixtures\": {\n";
	for (size_t fi = 0; fi < fixtures.size(); ++fi)
	{
		auto & f = fixtures[fi];
		std::vector<double> times;
		for (int i = 0; i < 30; ++i)
		{
			bytes restored;
			const auto start = clock_type::now();
			const bool ok = decode(l, f.selected, restored);
			const double us = std::chrono::duration<double, std::micro>(clock_type::now() - start).count();
			if (!ok || restored != f.raw) throw std::runtime_error("timed decode mismatch: " + f.name);
			samples << f.name << ',' << (i < 6 ? "warm" : "timed") << ',' << (i < 6 ? i : i - 6)
			        << ',' << us << ',' << f.selected.size() << ',' << f.raw.size() << ",1\n";
			if (i >= 6) times.push_back(us);
		}
		auto sorted = times;
		std::sort(sorted.begin(), sorted.end());
		auto percentile = [&](double q) { return sorted[size_t(std::ceil(sorted.size() * q)) - 1]; };
		summary << "    \"" << f.name << "\": {\"raw_bytes\": " << f.raw.size()
		        << ", \"selected_bytes\": " << f.selected.size()
		        << ", \"incumbent_bytes_before_native_row\": " << f.incumbent_bytes
		        << ", \"native_row_trial_bytes\": " << f.row_bytes
		        << ", \"native_row_savings_pct\": "
		        << (100.0 * (1.0 - double(f.row_bytes) / double(f.incumbent_bytes)))
		        << ", \"native_row_selected\": " << (f.row_selected ? "true" : "false")
		        << ", \"safety_raw_bytes\": " << f.safety_raw_bytes
		        << ", \"safety_wire_bytes\": " << f.safety.size()
		        << ", \"decode_p50_us\": " << percentile(.5)
		        << ", \"decode_p95_us\": " << percentile(.95) << "}"
		        << (fi + 1 == fixtures.size() ? "\n" : ",\n");
	}
	summary << "  }\n}\n";
	return 0;
}
catch (const std::exception & e)
{
	std::cerr << e.what() << '\n';
	return 1;
}
