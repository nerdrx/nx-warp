#include "../common/nxastc_packet_decode.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <lz4.h>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace wivrn::nxastc_packet;

static std::vector<uint8_t> zstd_bytes(std::span<const uint8_t> input, bool content_size = true)
{
	std::vector<uint8_t> out(ZSTD_compressBound(input.size()));
	ZSTD_CCtx * context = ZSTD_createCCtx();
	assert(context);
	assert(!ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, 3)));
	assert(!ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, content_size ? 1 : 0)));
	const size_t bytes = ZSTD_compress2(context, out.data(), out.size(), input.data(), input.size());
	ZSTD_freeCCtx(context);
	assert(!ZSTD_isError(bytes));
	out.resize(bytes);
	return out;
}

static std::vector<uint8_t> packet_bytes(uint32_t width, uint32_t height, compression encoding, std::span<const uint8_t> payload)
{
	auto header = make_header(width, height, uint32_t(payload.size()), encoding);
	std::vector<uint8_t> packet(header.begin(), header.end());
	packet.insert(packet.end(), payload.begin(), payload.end());
	return packet;
}

static std::vector<uint8_t> patterned_blocks(size_t blocks)
{
	std::vector<uint8_t> raw(blocks * 16);
	for (size_t i = 0; i < blocks; ++i)
	{
		uint8_t * b = raw.data() + i * 16;
		const uint32_t mode = (i & 1) ? compact_dual_mode : compact_single_mode;
		for (unsigned j = 0; j < 16; ++j)
			b[j] = uint8_t(((i / 8) + (j / 4)) & 7);
		b[0] = uint8_t(mode);
		b[1] = uint8_t(mode >> 8);
		b[2] = uint8_t((b[2] & 0xfeu) | ((mode >> 16) & 1u));
	}
	return raw;
}

static void randomized_block_roundtrips()
{
	std::mt19937_64 rng(0x4e584153544334ULL);
	for (unsigned trial = 0; trial < 2000; ++trial)
	{
		std::array<uint8_t, 32> raw{};
		std::array<uint8_t, 28> compact{};
		std::array<uint8_t, 32> restored{};
		for (size_t i = 0; i < raw.size(); ++i)
			raw[i] = uint8_t(rng());
		for (unsigned block = 0; block < 2; ++block)
		{
			const uint32_t mode = block == 0 ? compact_single_mode : compact_dual_mode;
			raw[block * 16] = uint8_t(mode);
			raw[block * 16 + 1] = uint8_t(mode >> 8);
			raw[block * 16 + 2] = uint8_t((raw[block * 16 + 2] & 0xfeu) | ((mode >> 16) & 1u));
		}
		assert(compact_blocks(raw, compact));
		std::copy(compact.begin(), compact.end(), restored.begin());
		assert(expand_compact_blocks(restored, compact.size()));
		assert(raw == restored);
	}

	std::array<uint8_t, 16> unknown{};
	std::array<uint8_t, 14> compact{};
	size_t compact_size = 0;
	assert(!compact_blocks(unknown, compact));
	assert(!compact_bytes_for_raw(15, compact_size));
	const size_t largest_aligned_raw = std::numeric_limits<size_t>::max() & ~size_t(15);
	assert(compact_bytes_for_raw(largest_aligned_raw, compact_size) && compact_size <= largest_aligned_raw);
	assert(!compact_blocks(unknown, std::span<uint8_t>(compact).first(13)));
	assert(!expand_compact_blocks(std::span<uint8_t>(unknown).first(15), 14));
	assert(!expand_compact_blocks(unknown, 13));
	assert(compact_block_bytes(UINT32_MAX, UINT32_MAX) < block_bytes(UINT32_MAX, UINT32_MAX));
}

static void legacy_packet_roundtrips(const std::vector<uint8_t> & raw)
{
	constexpr uint32_t width = 64, height = 64;
	std::vector<uint8_t> output(raw.size());
	auto raw_header = make_header(width, height, uint32_t(raw.size()), compression::none);
	assert(raw_header[4] == 1 && raw_header[5] == 0);
	auto raw_packet = packet_bytes(width, height, compression::none, raw);
	auto parsed = parse_packet(raw_packet);
	assert(parsed && decode_payload(*parsed, std::span<const uint8_t>(raw_packet).subspan(header_size), output) == decode_status::ok && output == raw);

	std::vector<uint8_t> lz4(LZ4_compressBound(int(raw.size())));
	const int lz4_size = LZ4_compress_default(reinterpret_cast<const char *>(raw.data()), reinterpret_cast<char *>(lz4.data()), int(raw.size()), int(lz4.size()));
	assert(lz4_size > 0);
	lz4.resize(size_t(lz4_size));
	auto lz4_packet = packet_bytes(width, height, compression::lz4, lz4);
	assert(lz4_packet[4] == 1 && lz4_packet[5] == 1);
	parsed = parse_packet(lz4_packet);
	assert(parsed && decode_payload(*parsed, std::span<const uint8_t>(lz4_packet).subspan(header_size), output) == decode_status::ok && output == raw);

	auto zstd = zstd_bytes(raw);
	auto zstd_packet = packet_bytes(width, height, compression::zstd, zstd);
	assert(zstd_packet[4] == 2 && zstd_packet[5] == 2);
	parsed = parse_packet(zstd_packet);
	assert(parsed && decode_payload(*parsed, std::span<const uint8_t>(zstd_packet).subspan(header_size), output) == decode_status::ok && output == raw);

	const auto anchor = make_motion_header(width, height, uint32_t(zstd.size()), compression::motion_zstd, independent_frame);
	assert(anchor[4] == 3 && anchor[5] == uint8_t(compression::motion_zstd));
	std::vector<uint8_t> anchor_packet(anchor.begin(), anchor.end());
	anchor_packet.insert(anchor_packet.end(), zstd.begin(), zstd.end());
	parsed = parse_packet(anchor_packet);
	assert(parsed && decode_motion_payload(*parsed, std::span<const uint8_t>(anchor_packet).subspan(motion_header_size), {}, output, {}) == decode_status::ok && output == raw);
}

static void compact_packet_cases()
{
	constexpr uint32_t width = 64, height = 64;
	const auto raw = patterned_blocks(64);
	const size_t compact_size = size_t(compact_block_bytes(width, height));
	std::vector<uint8_t> compact(compact_size);
	assert(compact_blocks(raw, compact));
	auto zstd = zstd_bytes(compact);
	assert(zstd.size() < compact.size());
	auto packet = packet_bytes(width, height, compression::compact_zstd, zstd);
	assert(packet[4] == 4 && packet[5] == uint8_t(compression::compact_zstd));
	auto parsed = parse_packet(packet);
	assert(parsed && parsed->raw_bytes == raw.size() && parsed->payload_bytes == zstd.size());
	std::vector<uint8_t> output(raw.size(), 0xa5);
	assert(decode_payload(*parsed, std::span<const uint8_t>(packet).subspan(header_size), output) == decode_status::ok);
	assert(output == raw);
	assert(decode_payload(*parsed, std::span<const uint8_t>(packet).subspan(header_size), std::span<uint8_t>(output).first(output.size() - 1)) == decode_status::length_mismatch);

	assert(!parse_packet(std::span<const uint8_t>(packet).first(header_size - 1)));
	bool rejected_size = false;
	try { (void)make_header(width, height, uint32_t(compact.size() + 1), compression::compact_zstd); }
	catch (const std::invalid_argument &) { rejected_size = true; }
	assert(rejected_size);
	rejected_size = false;
	try { (void)make_header(UINT32_MAX, UINT32_MAX, 1, compression::compact_zstd); }
	catch (const std::invalid_argument &) { rejected_size = true; }
	assert(rejected_size);

	// Strict Zstd frame validation also applies to the compact content length.
	auto truncated = zstd;
	truncated.pop_back();
	auto malformed = packet_bytes(width, height, compression::compact_zstd, truncated);
	parsed = parse_packet(malformed);
	assert(parsed && decode_payload(*parsed, std::span<const uint8_t>(malformed).subspan(header_size), output) == decode_status::zstd_bad_frame);

	auto trailing = zstd;
	trailing.push_back(0);
	malformed = packet_bytes(width, height, compression::compact_zstd, trailing);
	parsed = parse_packet(malformed);
	assert(parsed && decode_payload(*parsed, std::span<const uint8_t>(malformed).subspan(header_size), output) == decode_status::zstd_trailing_data);

	auto unknown_size = zstd_bytes(compact, false);
	malformed = packet_bytes(width, height, compression::compact_zstd, unknown_size);
	parsed = parse_packet(malformed);
	assert(parsed && decode_payload(*parsed, std::span<const uint8_t>(malformed).subspan(header_size), output) == decode_status::zstd_unknown_content_size);

	auto wrong_size = zstd_bytes(raw);
	malformed = packet_bytes(width, height, compression::compact_zstd, wrong_size);
	parsed = parse_packet(malformed);
	assert(parsed && decode_payload(*parsed, std::span<const uint8_t>(malformed).subspan(header_size), output) == decode_status::zstd_wrong_content_size);

	// The compact packet bound is 14 bytes per ASTC block, not 16.
	std::vector<uint8_t> oversized(header_size + compact.size() + 1);
	std::copy_n(packet.begin(), header_size, oversized.begin());
	write32(oversized.data() + 20, uint32_t(compact.size() + 1));
	assert(!parse_packet(oversized));

	for (auto [version, flag] : {std::pair<uint8_t,uint8_t>{4,0}, {4,2}, {5,5}, {2,5}})
	{
		auto bad = packet;
		bad[4] = version;
		bad[5] = flag;
		assert(!parse_packet(bad));
	}
}

static uint32_t read24(const uint8_t * p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
}

static int benchmark_photo(const char * path)
{
	std::ifstream file(path, std::ios::binary);
	std::vector<uint8_t> astc((std::istreambuf_iterator<char>(file)), {});
	if (astc.size() < 16 || astc[0] != 0x13 || astc[1] != 0xab || astc[2] != 0xa1 || astc[3] != 0x5c)
		return 2;
	const uint32_t width = read24(astc.data() + 7), height = read24(astc.data() + 10);
	std::span<const uint8_t> raw(astc.data() + 16, astc.size() - 16);
	const size_t expected = size_t(block_bytes(width, height));
	if (raw.size() != expected || raw.size() % 16)
		return 2;
	std::vector<uint8_t> compact(raw.size() / 16 * 14), base_out(ZSTD_compressBound(raw.size())), compact_out(ZSTD_compressBound(compact.size()));
	ZSTD_CCtx * base = ZSTD_createCCtx(), * candidate = ZSTD_createCCtx();
	assert(base && candidate);
	for (auto * c : {base, candidate})
	{
		assert(!ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, 3)));
		assert(!ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, 1)));
	}
	using clock = std::chrono::steady_clock;
	std::vector<double> base_us, pack_us, candidate_us;
	base_us.reserve(200); pack_us.reserve(200); candidate_us.reserve(200);
	for (unsigned i = 0; i < 220; ++i)
	{
		auto t0 = clock::now();
		size_t b = ZSTD_compress2(base, base_out.data(), base_out.size(), raw.data(), raw.size());
		auto t1 = clock::now();
		auto t2 = clock::now();
		bool ok = compact_blocks(raw, compact);
		auto t3 = clock::now();
		size_t c = ok ? ZSTD_compress2(candidate, compact_out.data(), compact_out.size(), compact.data(), compact.size()) : size_t(-1);
		auto t4 = clock::now();
		if (ZSTD_isError(b) || ZSTD_isError(c)) return 3;
		if (i >= 20)
		{
			base_us.push_back(std::chrono::duration<double, std::micro>(t1-t0).count());
			pack_us.push_back(std::chrono::duration<double, std::micro>(t3-t2).count());
			candidate_us.push_back(std::chrono::duration<double, std::micro>(t4-t2).count());
		}
	}
	auto stats = [](std::vector<double> v) {
		std::sort(v.begin(), v.end());
		return std::pair{v[v.size()/2], v[(v.size()*95)/100]};
	};
	auto [bp,b95] = stats(base_us); auto [pp,p95] = stats(pack_us); auto [cp,c95] = stats(candidate_us);
	const size_t bsize = ZSTD_compress2(base, base_out.data(), base_out.size(), raw.data(), raw.size());
	const size_t csize = ZSTD_compress2(candidate, compact_out.data(), compact_out.size(), compact.data(), compact.size());
	auto header = make_header(width, height, uint32_t(csize), compression::compact_zstd);
	std::vector<uint8_t> output(raw.size()), packet(header.begin(), header.end());
	packet.insert(packet.end(), compact_out.begin(), compact_out.begin() + csize);
	auto parsed = parse_packet(packet);
	if (!parsed) return 4;
	std::vector<double> decode_us;
	decode_us.reserve(200);
	for (unsigned i = 0; i < 220; ++i)
	{
		auto t0 = clock::now();
		auto status = decode_payload(*parsed, std::span<const uint8_t>(packet).subspan(header_size), output);
		auto t1 = clock::now();
		if (status != decode_status::ok || !std::equal(raw.begin(), raw.end(), output.begin())) return 5;
		if (i >= 20) decode_us.push_back(std::chrono::duration<double, std::micro>(t1-t0).count());
	}
	auto [dp,d95] = stats(decode_us);
	std::printf("%s %ux%u blocks=%zu bytes=%zu zstd=%zu compact+zstd=%zu saved=%.2f%% encode_us base=%.1f/%.1f pack=%.1f/%.1f candidate=%.1f/%.1f decode_inplace=%.1f/%.1f\n", path, width, height, raw.size()/16, raw.size(), bsize, csize, 100.0*(double(bsize)-double(csize))/double(bsize), bp,b95,pp,p95,cp,c95,dp,d95);
	ZSTD_freeCCtx(base); ZSTD_freeCCtx(candidate);
	return 0;
}

static int benchmark_decode_photo(const char * path)
{
	std::ifstream file(path, std::ios::binary);
	std::vector<uint8_t> astc((std::istreambuf_iterator<char>(file)), {});
	if (astc.size() < 16 || astc[0] != 0x13 || astc[1] != 0xab || astc[2] != 0xa1 || astc[3] != 0x5c) return 2;
	const uint32_t width = read24(astc.data() + 7), height = read24(astc.data() + 10);
	std::span<const uint8_t> raw(astc.data() + 16, astc.size() - 16);
	if (raw.size() != size_t(block_bytes(width, height))) return 2;
	std::vector<uint8_t> compact(raw.size() / 16 * 14), output(raw.size());
	if (!compact_blocks(raw, compact)) return 3;
	auto zstd = zstd_bytes(compact);
	auto compact_header = make_header(width, height, uint32_t(zstd.size()), compression::compact_zstd);
	std::vector<uint8_t> compact_packet(compact_header.begin(), compact_header.end());
	compact_packet.insert(compact_packet.end(), zstd.begin(), zstd.end());
	auto compact_parsed = parse_packet(compact_packet);
	auto baseline_zstd = zstd_bytes(raw);
	auto baseline_header = make_header(width, height, uint32_t(baseline_zstd.size()), compression::zstd);
	std::vector<uint8_t> baseline_packet(baseline_header.begin(), baseline_header.end());
	baseline_packet.insert(baseline_packet.end(), baseline_zstd.begin(), baseline_zstd.end());
	auto baseline_parsed = parse_packet(baseline_packet);
	if (!compact_parsed || !baseline_parsed) return 4;
	using clock = std::chrono::steady_clock;
	std::vector<double> times, baseline_times;
	times.reserve(200); baseline_times.reserve(200);
	for (unsigned i=0; i<220; ++i)
	{
		auto decode_one = [&](bool compact) {
			const auto & packet = compact ? compact_packet : baseline_packet;
			const auto & parsed = compact ? compact_parsed : baseline_parsed;
			const auto t0=clock::now();
			const auto status=decode_payload(*parsed, std::span<const uint8_t>(packet).subspan(header_size), output);
			const auto t1=clock::now();
			if (status != decode_status::ok || !std::equal(raw.begin(), raw.end(), output.begin()))
				return false;
			if (i>=20)
				(compact ? times : baseline_times).push_back(std::chrono::duration<double,std::micro>(t1-t0).count());
			return true;
		};
		// Alternate per-iteration order so the candidate never always runs first.
		if (!decode_one((i & 1) != 0) || !decode_one((i & 1) == 0)) return 5;
	}
	std::sort(times.begin(),times.end()); std::sort(baseline_times.begin(),baseline_times.end());
	std::printf("decode %s %ux%u astc=%zu base_zstd=%zu compact_zstd=%zu baseline_p50/p95=%.1f/%.1f compact_inplace_p50/p95=%.1f/%.1f us\n",path,width,height,raw.size(),baseline_zstd.size(),zstd.size(),baseline_times[100],baseline_times[190],times[100],times[190]);
	return 0;
}

int main(int argc, char ** argv)
{
	if (argc == 3 && std::string(argv[1]) == "--bench")
		return benchmark_photo(argv[2]);
	if (argc == 3 && std::string(argv[1]) == "--bench-decode")
		return benchmark_decode_photo(argv[2]);
	randomized_block_roundtrips();
	auto raw = patterned_blocks(64);
	legacy_packet_roundtrips(raw);
	compact_packet_cases();
	std::puts("NX ASTC compact v4, in-place expansion, malformed frames, and legacy v1-v3: PASS");
}
