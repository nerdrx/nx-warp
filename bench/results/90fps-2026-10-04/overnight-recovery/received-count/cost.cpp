#include "decoder/frame_window.h"
#include "decoder/nack_deadline.h"
#include "decoder/shard_set.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

using wivrn::shard_set;
using window_t = wivrn::frame_window<shard_set, 6, 3>;
using data_shard = wivrn::to_headset::video_stream_data_shard;
using bench_clock = std::chrono::steady_clock;

constexpr uint16_t shard_count = 521;
constexpr uint16_t hole_index = 519;
constexpr int64_t base_time = 1'000'000'000;
constexpr int64_t quiet = wivrn::nack_quiet_period_ns;

enum class scenario { interior, unknown_tail, parity };
struct fixture
{
	window_t window{shard_set(0)};
	std::vector<uint16_t> scratch;

	explicit fixture(scenario which)
	{
		auto noop = [](shard_set &) {};
		for (uint64_t frame = 0; frame < 6; ++frame)
		{
			shard_set * set = window.slot(frame, noop);
			const bool latest_unknown = which == scenario::unknown_tail and frame == 5;
			const bool hole = (which == scenario::interior or which == scenario::parity) and frame < 5;
			const bool has_end_marker = not latest_unknown;
			const bool frame_complete = has_end_marker and not hole;
			for (uint16_t i = 0; i < shard_count; ++i)
			{
				if (hole and i == hole_index)
					continue;
				data_shard shard{};
				shard.frame_idx = frame;
				shard.shard_idx = i;
				shard.payload = {};
				if (i == 0)
					shard.view_info.emplace();
				if (has_end_marker and i + 1 == shard_count)
					shard.timing_info.emplace();
				set->insert(std::move(shard), base_time);
			}
			if (which == scenario::parity and hole)
			{
				wivrn::to_headset::video_stream_parity_shard p{};
				p.first_shard_idx = hole_index - 1;
				p.shard_stride = 1;
				p.blob_size = {1, 1, 1};
				set->parity.push_back(std::move(p));
			}
			if (frame_complete)
				window.note_complete(frame);
		}
	}

	[[gnu::noinline]] std::optional<int64_t> deadline(int64_t now)
	{
		uint64_t newest = 0;
		bool any = false;
		window.for_each([&](shard_set & set) {
			if (set.empty())
				return;
			if (not any or set.frame_index() > newest)
			{
				newest = set.frame_index();
				any = true;
			}
		});
		if (not any)
			return {};

		std::optional<int64_t> earliest;
		window.for_each([&](shard_set & set) {
			auto due = wivrn::nack_poll_deadline(
			        now, set.last_shard, set.nack_last, set.nack_rounds,
			        true, true, not set.empty(), set.complete(), [&]() {
				        set.missing_shards(scratch, set.frame_index() < newest);
				        return not scratch.empty();
			        });
			if (due and (not earliest or *due < *earliest))
				earliest = *due;
		});
		return earliest;
	}
};

constexpr int iterations = 10'000;
constexpr int blocks = 5;

int main()
{
	std::puts("scenario,phase,block,iterations,ns_per_call,deadline_present");
	for (scenario which: {scenario::interior, scenario::unknown_tail, scenario::parity})
	{
		const char * name = which == scenario::interior ? "late_interior_hole" :
		                    which == scenario::unknown_tail ? "unknown_tail" : "parity_suppressed";
		fixture f(which);
		volatile int64_t sink = 0;
		for (bool due: {false, true})
			for (int i = 0; i < 1'000; ++i)
				sink += f.deadline(base_time + quiet - (due ? 0 : 1)).has_value();
		for (int block = 0; block < blocks; ++block)
		{
			for (int phase = 0; phase < 2; ++phase)
			{
				const bool due = ((block + phase) % 2) == 1;
				const int64_t now = base_time + quiet - (due ? 0 : 1);
				auto start = bench_clock::now();
				std::optional<int64_t> result;
				for (int i = 0; i < iterations; ++i)
				{
					asm volatile("" ::: "memory");
					result = f.deadline(now);
				}
				auto end = bench_clock::now();
				sink += result ? *result : 0;
				const bool expected = which == scenario::interior or not due;
				if (bool(result) != expected)
					return 2;
				const double ns = std::chrono::duration<double, std::nano>(end - start).count() / iterations;
				std::printf("%s,%s,%d,%d,%.1f,%d\n", name, due ? "due" : "before_due", block, iterations, ns, bool(result));
			}
		}
		if (sink == -1)
			std::puts("unreachable");
	}
}
