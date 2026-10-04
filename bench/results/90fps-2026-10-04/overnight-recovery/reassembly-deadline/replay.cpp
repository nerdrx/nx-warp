// Deterministic arrival replay over production frame_window and shard_set.
#include "frame_window.h"
#include "shard_set.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
using set_t = wivrn::shard_set;
using window_t = wivrn::frame_window<set_t, 6, 3>;
using data_shard = wivrn::to_headset::video_stream_data_shard;
constexpr int64_t panel_period = 1'000'000'000 / 90;
constexpr int64_t epoch = 10'000'000'000LL;
constexpr uint64_t frame_count = 80;

struct packet
{
	int64_t time;
	uint64_t frame;
	uint16_t shard;
};

data_shard make_shard(uint64_t frame, uint16_t index)
{
	data_shard s{};
	s.frame_idx = frame;
	s.shard_idx = index;
	if (index == 0)
		s.view_info.emplace();
	if (index == 3)
		s.timing_info.emplace();
	return s;
}

std::vector<packet> arrivals(int source_fps)
{
	const int64_t source_period = 1'000'000'000LL / source_fps;
	std::vector<packet> out;
	for (uint64_t frame = 0; frame < frame_count; ++frame)
	{
		int64_t offsets[] = {0, 250'000, 500'000, 750'000};
		if (frame % 4 == 2)
			std::swap(offsets[1], offsets[2]); // deterministic within-frame reorder
		if (frame % 3 == 1)
			offsets[3] += 5'000'000; // deterministic trickle of end shard
		for (uint16_t shard = 0; shard < 4; ++shard)
		{
			if (frame % 7 == 0 and shard == 3)
				continue; // one lost end shard; frame remains incomplete
			out.push_back({epoch + int64_t(frame) * source_period + offsets[shard], frame, shard});
		}
	}
	std::stable_sort(out.begin(), out.end(), [](const packet & a, const packet & b) { return a.time < b.time; });
	return out;
}

struct result
{
	uint64_t complete_delivered = 0;
	uint64_t incomplete_retired = 0;
	uint64_t too_old = 0;
	std::vector<int64_t> retire_age;
};

result replay(int source_fps, bool deadline)
{
	window_t window{set_t{}};
	result r;
	int64_t now = epoch;
	auto retire = [&](set_t & set) {
		if (not set.complete() and set.feedback.received_first_packet > 0)
		{
			++r.incomplete_retired;
			r.retire_age.push_back(now - set.feedback.received_first_packet);
		}
	};
	for (const packet & p: arrivals(source_fps))
	{
		now = p.time;
		set_t * set = window.slot(p.frame, retire);
		if (not set)
		{
			++r.too_old;
			continue;
		}
		set->insert(make_shard(p.frame, p.shard), now);
		if (set->complete())
			window.note_complete(p.frame);
		auto visit = [&](set_t & front) {
			if (not front.complete())
				return window_t::step::wait;
			++r.complete_delivered;
			return window_t::step::done;
		};
		if (deadline)
			window.drain(visit, retire, [&](const set_t & front) {
				return wivrn::frame_deadline_expired(now, front.feedback.received_first_packet,
			                                             panel_period, true,
			                                             window.has_newer_complete_than_front());
			});
		else
			window.drain(visit, retire);
	}
	return r;
}

double percentile(std::vector<int64_t> values, double p)
{
	if (values.empty())
		return 0;
	std::sort(values.begin(), values.end());
	return double(values[static_cast<size_t>((values.size() - 1) * p)]) / 1e6;
}

void print(int fps, const char * mode, const result & r)
{
	std::printf("%2d %8s %3llu %3llu %7.2f %7.2f %7.2f %3llu\n", fps, mode,
	            static_cast<unsigned long long>(r.complete_delivered),
	            static_cast<unsigned long long>(r.incomplete_retired),
	            percentile(r.retire_age, 0.50), percentile(r.retire_age, 0.90),
	            percentile(r.retire_age, 1.00), static_cast<unsigned long long>(r.too_old));
}
} // namespace

int main()
{
	std::puts("source mode complete incomplete-retired age-p50-ms age-p90-ms age-max-ms old-shards");
	for (int fps: {30, 60, 90})
	{
		const result baseline = replay(fps, false);
		const result deadline = replay(fps, true);
		if (baseline.complete_delivered != 66 or baseline.incomplete_retired != 11 or
		    deadline.complete_delivered != 68 or deadline.incomplete_retired != 12 or
		    baseline.too_old != 0 or deadline.too_old != 0)
			return 1;
		print(fps, "skew3", baseline);
		print(fps, "2period", deadline);
	}
	return 0;
}
