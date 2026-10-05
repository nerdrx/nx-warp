#include "fec.h"
#include "shard_history.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <span>
#include <thread>
#include <vector>

using namespace wivrn;

namespace
{
constexpr uint64_t frames = 20'000;
constexpr uint32_t shard_count = 4;
constexpr size_t payload_size = 96;
std::atomic<uint64_t> published{0};
std::atomic<bool> done{false};
std::atomic<uint64_t> hits{0}, misses{0}, failures{0};

uint8_t byte_for(uint64_t frame, uint16_t shard, size_t offset)
{
	return uint8_t((frame * 29 + uint64_t(shard) * 71 + offset * 13 + (frame >> 8)) & 0xff);
}

std::vector<uint8_t> payload_for(uint64_t frame, uint16_t shard)
{
	std::vector<uint8_t> payload(payload_size);
	for (size_t i = 0; i < payload.size(); ++i)
		payload[i] = byte_for(frame, shard, i);
	for (size_t i = 0; i < sizeof(frame); ++i)
		payload[i] = uint8_t(frame >> (i * 8));
	payload[8] = uint8_t(shard);
	payload[9] = uint8_t(shard >> 8);
	return payload;
}

void writer(shard_history & history)
{
	std::vector<uint8_t> payload, blob;
	for (uint64_t frame = 1; frame <= frames; ++frame)
	{
		for (uint16_t index = 0; index < shard_count; ++index)
		{
			payload = payload_for(frame, index);
			to_headset::video_stream_data_shard shard{};
			shard.frame_idx = frame;
			shard.shard_idx = index;
			if (index == 0)
				shard.view_info.emplace();
			shard.payload = payload;
			if (index + 1 == shard_count)
				shard.timing_info = to_headset::video_stream_data_shard::timing_info_t{
				        .encode_begin = XrTime(frame * 100 + 1),
				        .encode_end = XrTime(frame * 100 + 2),
				        .send_begin = XrTime(frame * 100 + 3),
				        .send_end = XrTime(frame * 100 + 4),
			};
			fec::encode_blob(shard, blob);
			history.push(frame, index, blob, true);
		}
		history.end_frame(frame, shard_count);
		published.store(frame, std::memory_order_release);
	}
	done.store(true, std::memory_order_release);
}

void reader(shard_history & history, uint64_t salt)
{
	uint64_t n = 0;
	while (!done.load(std::memory_order_acquire) || n < frames)
	{
		const uint64_t top = published.load(std::memory_order_acquire);
		if (top == 0)
			continue;
		// Mix latest and older frame IDs to cross the ring and entry-count eviction edges.
		const uint64_t lag = (n * 97 + salt * 211) % 1800;
		const uint64_t frame = top > lag ? top - lag : top;
		auto candidate = history.collect_frame_end_candidate(frame, {}, 1);
		if (!candidate)
		{
			++misses;
			++n;
			continue;
		}
		auto shard = fec::decode_blob(0, frame, candidate->shard_idx, candidate->blob);
		bool good = candidate->shard_idx == shard_count - 1 && shard.frame_idx == frame &&
		            shard.shard_idx == shard_count - 1 && shard.timing_info.has_value() &&
		            shard.payload.size() == payload_size;
	for (size_t i = 0; good && i < sizeof(frame); ++i)
		good = shard.payload[i] == uint8_t(frame >> (i * 8));
	good = good && shard.payload[8] == uint8_t(shard_count - 1) && shard.payload[9] == 0;
	for (size_t i = 10; good && i < shard.payload.size(); ++i)
		good = shard.payload[i] == byte_for(frame, shard_count - 1, i);
	if (good)
	{
		const auto & timing = *shard.timing_info;
		good = timing.encode_begin == XrTime(frame * 100 + 1) &&
		       timing.encode_end == XrTime(frame * 100 + 2) &&
		       timing.send_begin == XrTime(frame * 100 + 3) &&
		       timing.send_end == XrTime(frame * 100 + 4);
	}
		if (!good)
			++failures;
		else
			++hits;
		++n;
	}
}

void toggler(shard_history & history)
{
	for (int i = 0; i < 500; ++i)
	{
		history.set_enabled(false);
		history.set_enabled(true);
		std::this_thread::yield();
	}
}
} // namespace

int main()
{
	shard_history history;
	history.set_enabled(true);
	std::thread producer(writer, std::ref(history));
	std::thread left(reader, std::ref(history), 1);
	std::thread right(reader, std::ref(history), 2);
	std::thread settings(toggler, std::ref(history));
	producer.join();
	left.join();
	right.join();
	settings.join();
	const bool bounded = history.held() <= shard_history::max_entries &&
	                     history.bytes() == shard_history::capacity &&
	                     published.load(std::memory_order_acquire) == frames && hits.load() > 0;
	std::printf("frames=%llu shards/frame=%u reader_hits=%llu reader_misses=%llu identity_failures=%llu held=%zu bytes=%zu\n",
	            (unsigned long long)frames, shard_count,
	            (unsigned long long)hits.load(), (unsigned long long)misses.load(),
	            (unsigned long long)failures.load(), history.held(), history.bytes());
	return failures.load() || !bounded ? 1 : 0;
}
