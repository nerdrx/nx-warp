#include "../client/decoder/astc/packet_buffer_recycle.h"
#include "../common/nxastc_packet.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <zstd.h>

namespace
{
using bytes = std::vector<uint8_t>;
using clock_type = std::chrono::steady_clock;
using namespace wivrn::astc_detail;
using namespace wivrn::nxastc_packet;

void policy_test()
{
	bytes spare;
	bytes packet;
	packet.reserve(32);
	packet.assign(16, 0x31);
	recycle_packet_buffer_locked(spare, packet, packet.capacity());
	assert(spare.capacity() >= 32 && spare.empty());
	assert(packet.capacity() == 0 && packet.empty());

	// Empty assemblers take the spare; non-empty assemblers remain untouched.
	bytes assembling;
	use_recycled_packet_buffer_locked(assembling, spare);
	assert(assembling.capacity() >= 32 && spare.capacity() == 0);
	assembling.assign(4, 0x42);
	spare.reserve(48);
	spare.assign(12, 0x53);
	use_recycled_packet_buffer_locked(assembling, spare);
	assert(assembling == bytes(4, 0x42) && spare == bytes(12, 0x53));

	// Keep the larger spare and preserve rejected candidates byte-for-byte.
	packet.clear();
	packet.reserve(24);
	packet.assign(8, 0x64);
	recycle_packet_buffer_locked(spare, packet, 64);
	assert(spare == bytes(12, 0x53) && packet == bytes(8, 0x64));

	// Replace with a larger packet buffer, but reject capacity above the bound.
	packet.clear();
	packet.reserve(64);
	packet.assign(20, 0x75);
	const size_t packet_capacity = packet.capacity();
	const size_t old_spare_capacity = spare.capacity();
	recycle_packet_buffer_locked(spare, packet, packet_capacity);
	assert(spare.capacity() == packet_capacity && spare.empty());
	assert(packet.capacity() == old_spare_capacity && packet.empty());

	bytes oversized;
	oversized.reserve(packet_capacity + 1);
	oversized.assign(10, 0x86);
	recycle_packet_buffer_locked(spare, oversized, packet_capacity);
	assert(oversized == bytes(10, 0x86) && spare.capacity() == packet_capacity);
	bytes empty;
	recycle_packet_buffer_locked(spare, empty, packet_capacity);
	assert(spare.capacity() == packet_capacity && empty.capacity() == 0);
}

void threaded_handoff_test()
{
	constexpr size_t iterations = 10000;
	constexpr size_t cap = 128;
	std::mutex mutex;
	std::condition_variable wake;
	bytes spare, assembling, in_flight;
	bool ready = false;
	size_t reused = 0;

	std::thread worker([&] {
		for (size_t i = 0; i < iterations; ++i)
		{
			std::unique_lock lock(mutex);
			wake.wait(lock, [&] { return ready; });
			assert(in_flight.size() == 64 && in_flight.front() == uint8_t(i) && in_flight.back() == uint8_t(i));
			recycle_packet_buffer_locked(spare, in_flight, cap);
			ready = false;
			lock.unlock();
			wake.notify_one();
		}
	});
	for (size_t i = 0; i < iterations; ++i)
	{
		std::unique_lock lock(mutex);
		wake.wait(lock, [&] { return !ready; });
		use_recycled_packet_buffer_locked(assembling, spare);
		if (assembling.capacity() != 0)
			++reused;
		assembling.assign(64, uint8_t(i));
		in_flight = std::move(assembling);
		ready = true;
		lock.unlock();
		wake.notify_one();
	}
	worker.join();
	assert(reused == iterations - 1);
}

struct append_stats
{
	size_t growths = 0;
	size_t relocated_bytes = 0;
};

using fragments = std::array<std::span<const uint8_t>, 270>;

fragments fragment_packet(const bytes & packet)
{
	fragments out;
	const size_t base = packet.size() / out.size();
	const size_t remainder = packet.size() % out.size();
	size_t offset = 0;
	for (size_t i = 0; i < out.size(); ++i)
	{
		const size_t length = base + (i < remainder);
		out[i] = std::span<const uint8_t>(packet).subspan(offset, length);
		offset += length;
	}
	assert(offset == packet.size());
	return out;
}

void append_270(bytes & assembling, const fragments & parts, append_stats & stats)
{
	for (const auto part: parts)
	{
		const size_t old_capacity = assembling.capacity();
		const size_t old_size = assembling.size();
		assembling.insert(assembling.end(), part.begin(), part.end());
		if (assembling.capacity() != old_capacity)
		{
			++stats.growths;
			stats.relocated_bytes += old_size;
		}
	}
}

bool matches_fragments(std::span<const uint8_t> bytes, const fragments & parts)
{
	size_t offset = 0;
	for (const auto part: parts)
	{
		if (part.size() > bytes.size() - offset || !std::equal(part.begin(), part.end(), bytes.begin() + offset))
			return false;
		offset += part.size();
	}
	return offset == bytes.size();
}

double percentile(std::vector<double> values, double p)
{
	std::sort(values.begin(), values.end());
	return values[size_t((values.size() - 1) * p)];
}

bytes read_native_packet(const std::string & path, const char * name)
{
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input)
		throw std::runtime_error("cannot open " + path);
	const auto file_size = input.tellg();
	if (file_size < 16)
		throw std::runtime_error("short ASTC fixture " + path);
	bytes file(static_cast<size_t>(file_size));
	input.seekg(0);
	input.read(reinterpret_cast<char *>(file.data()), file.size());
	const std::array<uint8_t, 4> magic{0x13, 0xab, 0xa1, 0x5c};
	const auto read24 = [&](size_t offset) {
		return uint32_t(file[offset]) | (uint32_t(file[offset + 1]) << 8) | (uint32_t(file[offset + 2]) << 16);
	};
	if (!input || !std::equal(magic.begin(), magic.end(), file.begin()) || file[4] != 8 || file[5] != 8 ||
	    file[6] != 1 || read24(7) != 2176 || read24(10) != 2176 || read24(13) != 1)
		throw std::runtime_error("invalid ASTC fixture header " + path);
	bytes raw(file.begin() + 16, file.end());
	constexpr uint32_t dimension = 2176;
	if (raw.size() != block_bytes(dimension, dimension))
		throw std::runtime_error("unexpected native fixture dimensions " + path);

	bytes compressed(ZSTD_compressBound(raw.size()));
	const size_t compressed_size = ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 3);
	if (ZSTD_isError(compressed_size))
		throw std::runtime_error(ZSTD_getErrorName(compressed_size));
	compressed.resize(compressed_size);
	bytes restored(raw.size());
	const size_t restored_size = ZSTD_decompress(restored.data(), restored.size(), compressed.data(), compressed.size());
	if (ZSTD_isError(restored_size) || restored_size != raw.size() || restored != raw)
		throw std::runtime_error("Zstd fixture roundtrip failed " + path);
	const auto header = make_header(dimension, dimension, uint32_t(compressed.size()), compression::zstd);
	bytes packet(header.begin(), header.end());
	packet.insert(packet.end(), compressed.begin(), compressed.end());
	const auto parsed = parse_packet(packet);
	if (!parsed || parsed->encoding != compression::zstd || parsed->raw_bytes != raw.size())
		throw std::runtime_error("invalid synthetic NX ASTC packet");
	std::cout << name << " raw=" << raw.size() << " packet=" << packet.size() << " zstd=" << compressed_size << '\n';
	return packet;
}

struct bench_result
{
	append_stats stats;
	std::vector<double> frame_us;
};

bench_result baseline_bench(const fragments & parts, size_t iterations)
{
	bench_result result;
	std::mutex mutex;
	bytes assembling;
	for (size_t i = 0; i < iterations; ++i)
	{
		bytes completed;
		const auto start = clock_type::now();
		{
			std::lock_guard lock(mutex);
			append_270(assembling, parts, result.stats);
			completed = std::move(assembling);
		}
		const auto end = clock_type::now();
		if (!matches_fragments(completed, parts))
			throw std::runtime_error("baseline assembly validation failed");
		result.frame_us.push_back(std::chrono::duration<double, std::micro>(end - start).count());
	}
	return result;
}

bench_result recycle_bench(const fragments & parts, size_t max_packet, size_t iterations)
{
	bench_result result;
	std::mutex mutex;
	bytes spare, assembling, worker_packet;
	for (size_t i = 0; i < iterations; ++i)
	{
		bytes completed;
		const auto start = clock_type::now();
		{
			std::lock_guard lock(mutex);
			use_recycled_packet_buffer_locked(assembling, spare);
			append_270(assembling, parts, result.stats);
			completed = std::move(assembling);
			use_recycled_packet_buffer_locked(assembling, spare);
		}
		const auto end = clock_type::now();
		// A full comparison outside the timed region keeps the payload copies observable.
		if (!matches_fragments(completed, parts))
			throw std::runtime_error("recycled assembly validation failed");
		result.frame_us.push_back(std::chrono::duration<double, std::micro>(end - start).count());

		// Model one completed worker frame becoming reusable after the current frame
		// was assembled, so the producer can consume it on the next frame boundary.
		if (!worker_packet.empty())
		{
			std::lock_guard lock(mutex);
			recycle_packet_buffer_locked(spare, worker_packet, max_packet);
		}
		worker_packet = std::move(completed);
	}
	return result;
}

void print_bench(const char * mode, const bench_result & result, size_t skip)
{
	std::vector<double> steady(result.frame_us.begin() + skip, result.frame_us.end());
	std::cout << mode << " frames=" << result.frame_us.size() << " allocations_by_capacity_growth=" << result.stats.growths
	          << " relocated_bytes=" << result.stats.relocated_bytes << " producer_us_p50/p95=" << percentile(steady, .50) << '/'
	          << percentile(steady, .95) << '\n';
}

void benchmark(const std::string & dark_path, const std::string & forest_path)
{
	constexpr size_t iterations = 1000;
	for (const auto & [name, path]: {std::pair{"dark", dark_path}, std::pair{"forest", forest_path}})
	{
		const bytes packet = read_native_packet(path, name);
		const auto parts = fragment_packet(packet);
		const size_t cap = size_t(block_bytes(2176, 2176)) + motion_header_size;
		auto baseline = baseline_bench(parts, iterations);
		auto recycled = recycle_bench(parts, cap, iterations);
		print_bench("current", baseline, 0);
		print_bench("one-frame-lag recycle", recycled, 2);
	}
}
} // namespace

int main(int argc, char ** argv)
{
	policy_test();
	threaded_handoff_test();
	std::cout << "ASTC packet buffer recycle policy + threaded handoff: PASS\n";
	if (argc == 3)
		benchmark(argv[1], argv[2]);
	else if (argc != 1)
		throw std::runtime_error("usage: astc_packet_buffer_recycle_test [dark-q6.astc forest-q6.astc]");
}
