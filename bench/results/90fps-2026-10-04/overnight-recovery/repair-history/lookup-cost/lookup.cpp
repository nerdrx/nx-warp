#include "fec.h"

#define shard_history history_1m
#include "../baseline/shard_history.h"
#undef shard_history
#define shard_history history_2m
#include "../candidate/shard_history.h"
#undef shard_history

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using wivrn::to_headset::video_stream_data_shard;
using clock_type = std::chrono::steady_clock;
using history_1m = wivrn::history_1m;
using history_2m = wivrn::history_2m;

struct histories
{
	history_1m one;
	history_2m two;
	uint16_t shards = 0;

	histories()
	{
		one.set_enabled(true);
		two.set_enabled(true);
		constexpr size_t frame_bytes = 1'000'000'000ULL / 8 / 90 / 2;
		std::vector<uint8_t> payload(frame_bytes), blob;
		for (uint64_t frame = 10; frame <= 12; ++frame)
		{
			std::fill(payload.begin(), payload.end(), uint8_t(frame));
			size_t offset = 0;
			uint16_t index = 0;
			while (offset < frame_bytes)
			{
				video_stream_data_shard shard{};
				shard.frame_idx = frame;
				shard.shard_idx = index;
				if (index == 0)
					shard.view_info.emplace();
				size_t size = std::min(frame_bytes - offset,
				                       wivrn::fec::shard_payload_budget(true) - wivrn::serialized_size(shard.view_info));
				shard.payload = std::span(payload).subspan(offset, size);
				if (offset + size == frame_bytes)
					shard.timing_info.emplace();
				wivrn::fec::encode_blob(shard, blob);
				one.push(frame, index, blob, true);
				two.push(frame, index, blob, true);
				offset += size;
				++index;
			}
			shards = index;
		}
		assert(one.bytes() == history_1m::capacity);
		assert(two.bytes() == history_2m::capacity);
	}
};

template <typename History>
size_t collect(History & h, uint64_t frame, std::vector<typename History::hit> & hits)
{
	hits.clear();
	constexpr std::array<uint8_t, 8> bitmap{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	return h.collect(frame, 0, bitmap, 64, hits);
}

template <typename History>
void check_fixture(History & h, uint16_t expected_shards)
{
	std::vector<typename History::hit> hits;
	assert(collect(h, 12, hits) == 64);
	assert(hits.size() == 64);
	size_t bytes = 0;
	for (const auto & hit: hits)
	{
		auto shard = wivrn::fec::decode_blob(0, 12, hit.shard_idx, hit.blob);
		assert(std::all_of(shard.payload.begin(), shard.payload.end(), [](uint8_t b) { return b == 12; }));
		bytes += shard.payload.size();
	}
	assert(bytes != 0);
	assert(expected_shards > 64);
	assert(collect(h, 9, hits) == 0);
	assert(hits.empty());
}

struct result
{
	double ns_per_op;
	size_t found;
};

template <typename History>
result measure(History & h, uint64_t frame, size_t repetitions)
{
	std::vector<typename History::hit> hits;
	collect(h, frame, hits); // warm output storage and cache path
	const auto start = clock_type::now();
	size_t found = 0;
	for (size_t i = 0; i < repetitions; ++i)
		found += collect(h, frame, hits);
	const auto elapsed = std::chrono::duration<double, std::nano>(clock_type::now() - start).count();
	return {.ns_per_op = elapsed / repetitions, .found = found / repetitions};
}

int main()
{
	constexpr size_t repetitions = 250;
	histories h;
	check_fixture(h.one, h.shards);
	check_fixture(h.two, h.shards);
	std::puts("block,order,capacity,query,ns_per_op,mean_found,held_entries");
	for (int block = 0; block < 5; ++block)
	{
		const bool reverse = block % 2;
		for (int position = 0; position < 4; ++position)
		{
			const bool use_two = reverse ? position == 0 || position == 3 : position == 1 || position == 2;
			const char * order = reverse ? "2M-1M-1M-2M" : "1M-2M-2M-1M";
			for (int query = 0; query < 2; ++query)
			{
				const uint64_t frame = query == 0 ? 12 : 9;
				const char * name = query == 0 ? "current_hit64" : "absent_miss";
				result r = use_two ? measure(h.two, frame, repetitions) : measure(h.one, frame, repetitions);
				const size_t held = use_two ? h.two.held() : h.one.held();
				std::printf("%d,%s,%s,%s,%.1f,%zu,%zu\n", block + 1, order, use_two ? "2MiB" : "1MiB", name, r.ns_per_op, r.found, held);
			}
		}
	}
}
