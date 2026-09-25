// Host-only size/timing comparison for captured raw NXDF frames.
#include "nxwarp_direct_checkerboard.h"
#include "nxwarp_direct_lz4.h"
#include "nxwarp_direct_zstd.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace wivrn::nxwarp_direct;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

static double median(std::vector<double> values)
{
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}

static Bytes read_file(const char * path)
{
	std::ifstream f(path, std::ios::binary);
	return Bytes(std::istreambuf_iterator<char>(f), {});
}

static void report(const char * path, const char * codec, size_t raw_bytes,
                   std::vector<double> baseline_ms, std::vector<double> checker_ms,
                   size_t baseline_bytes, size_t checker_bytes)
{
	baseline_ms.erase(baseline_ms.begin(), baseline_ms.begin() + 4);
	checker_ms.erase(checker_ms.begin(), checker_ms.begin() + 4);
	std::printf("%s,%s,raw=%zu,baseline=%zu,checker=%zu,saved=%zd,baseline_p50_ms=%.4f,checker_p50_ms=%.4f\n",
	            path, codec, raw_bytes, baseline_bytes, checker_bytes,
	            ssize_t(baseline_bytes) - ssize_t(checker_bytes), median(baseline_ms), median(checker_ms));
}

int main(int argc, char ** argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s captured-raw-frame.nxdf [...]\n", argv[0]);
		return 2;
	}
	for (int arg = 1; arg < argc; ++arg) {
		const Bytes raw = read_file(argv[arg]);
		layout l{2176, 2176, 2, true, 256, false, false, false, true};
		if (raw.empty() || !parse_frame(l, raw)) {
			std::fprintf(stderr, "invalid 2176x2176 RGB888 NXDF frame: %s\n", argv[arg]);
			return 2;
		}
		for (uint32_t phase = 0; phase != 2; ++phase) {
			Bytes checker_storage;
			const auto checker = checkerboard_frame(l, raw, checker_storage, phase);
			if (checker.empty() || !parse_frame(l, checker)) return 1;
			Bytes base_lz4_storage, checker_lz4_storage, base_zstd_storage, checker_zstd_storage;
			std::vector<double> base_lz4_ms, checker_lz4_ms, base_zstd_ms, checker_zstd_ms;
			for (unsigned i = 0; i < 36; ++i) {
				auto start = Clock::now();
				auto packed_base = compress_lz4(raw, base_lz4_storage);
				base_lz4_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
				start = Clock::now();
				auto packed_checker = compress_lz4(checker, checker_lz4_storage);
				checker_lz4_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
				start = Clock::now();
				auto zstd_base = compress_zstd(raw, base_zstd_storage);
				base_zstd_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
				start = Clock::now();
				auto zstd_checker = compress_zstd(checker, checker_zstd_storage);
				checker_zstd_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
				if (i == 35) {
					Bytes decoded;
					auto exact_lz4 = [&](std::span<const uint8_t> packed, std::span<const uint8_t> expected) {
						if (is_lz4(packed)) return decompress_lz4(l, packed, decoded) && decoded == Bytes(expected.begin(), expected.end());
						return std::equal(packed.begin(), packed.end(), expected.begin(), expected.end());
					};
					auto exact_zstd = [&](std::span<const uint8_t> packed, std::span<const uint8_t> expected) {
						if (is_zstd(packed)) return decompress_zstd(l, packed, decoded) && decoded == Bytes(expected.begin(), expected.end());
						return std::equal(packed.begin(), packed.end(), expected.begin(), expected.end());
					};
					if (!exact_lz4(packed_base, raw) || !exact_lz4(packed_checker, checker) ||
					    !exact_zstd(zstd_base, raw) || !exact_zstd(zstd_checker, checker)) return 1;
					report(argv[arg], phase ? "lz4-phase1" : "lz4-phase0", raw.size(), base_lz4_ms, checker_lz4_ms, packed_base.size(), packed_checker.size());
					report(argv[arg], phase ? "zstd-phase1" : "zstd-phase0", raw.size(), base_zstd_ms, checker_zstd_ms, zstd_base.size(), zstd_checker.size());
				}
			}
		}
	}
}
