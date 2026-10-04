// Deterministic two-eye replay. Window/reassembly is production code; pair
// selection below is an idealized exact-ID intersection, not renderer code.
#include "frame_window.h"
#include "shard_set.h"
#include "utils/retained_frame_slot.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

namespace
{
using set_t = wivrn::shard_set;
using window_t = wivrn::frame_window<set_t, 6, 3>;
using data_shard = wivrn::to_headset::video_stream_data_shard;
constexpr int64_t display_period = 1'000'000'000 / 90;
constexpr int64_t epoch = 10'000'000'000LL;
constexpr size_t retained_count = 3; // stream.cpp default; fourth slot is opt-in

data_shard packet(uint64_t frame, uint16_t shard)
{
	data_shard p{};
	p.frame_idx = frame;
	p.shard_idx = shard;
	if (shard == 0) p.view_info.emplace();
	if (shard == 1) p.timing_info.emplace();
	return p;
}

struct eye
{
	window_t window;
	std::array<std::optional<uint64_t>, 4> retained{};
	std::vector<uint64_t> decoded;
	std::vector<std::pair<uint64_t, int64_t>> retired;
	uint64_t too_old = 0;
	bool deadline;
	int64_t now = epoch;

	explicit eye(bool use_deadline) : window(set_t{}), deadline(use_deadline) {}

	bool push(uint64_t frame, uint16_t shard, int64_t at)
	{
		now = at;
		set_t * set = window.slot(frame, [&](set_t & old) {
			if (not old.complete()) retired.emplace_back(old.frame_index(), now);
		});
		if (not set)
		{
			++too_old;
			return false;
		}
		set->insert(packet(frame, shard), now);
		if (set->complete()) window.note_complete(frame);
		auto visit = [&](set_t & front) {
			if (not front.complete()) return window_t::step::wait;
			decoded.push_back(front.frame_index());
			std::array<std::optional<uint64_t>, 4> ids = retained;
			if (auto slot = wivrn::retained_frame_slot(std::span(ids).first(retained_count), front.frame_index()))
			{
				retained[*slot] = front.frame_index();
			}
			return window_t::step::done;
		};
		auto retire = [&](set_t & old) {
			if (not old.complete()) retired.emplace_back(old.frame_index(), now);
		};
		if (deadline)
			window.drain(visit, retire, [&](const set_t & front) {
				return wivrn::frame_deadline_expired(now, front.feedback.received_first_packet,
			                                             display_period, true,
			                                             window.has_newer_complete_than_front());
			});
		else
			window.drain(visit, retire);
		return true;
	}

	std::optional<int64_t> retirement_time(uint64_t frame) const
	{
		for (auto [id, at]: retired) if (id == frame) return at;
		return {};
	}
};

struct trace
{
	eye left;
	eye right;
	std::optional<uint64_t> current;
	std::vector<uint64_t> updates;
	uint64_t held_refreshes = 0;
	int64_t end = epoch;
	int64_t next_refresh = epoch + display_period;

	explicit trace(bool deadline) : left(deadline), right(deadline) {}

	void render_through(int64_t through)
	{
		while (next_refresh <= through)
		{
			std::optional<uint64_t> newest_common;
			for (auto l: std::span(left.retained).first(retained_count))
				if (l and (not current or *l >= *current) and
				    std::find(right.retained.begin(), right.retained.begin() + retained_count, l) !=
			                    right.retained.begin() + retained_count)
					newest_common = std::max(newest_common.value_or(*l), *l);
			if (newest_common and (not current or *newest_common > *current))
			{
				current = newest_common;
				updates.push_back(*current);
			}
			else
				++held_refreshes;
			next_refresh += display_period;
		}
	}
};

int trigger_frame(int fps)
{
	const int64_t source_period = 1'000'000'000LL / fps;
	return source_period >= 2 * display_period ? 2 : 3;
}

struct result
{
	trace replay;
	int64_t trigger_at;
	bool repair_left_accepted;
	bool repair_right_accepted;

	explicit result(bool deadline, int64_t trigger) : replay(deadline), trigger_at(trigger) {}
};

struct event
{
	int64_t at;
	int eye_index;
	uint64_t frame;
	uint16_t shard;
};

result run(int fps, bool deadline, int64_t repair_left, int64_t repair_right)
{
	const int64_t p = 1'000'000'000LL / fps;
	const int successor = trigger_frame(fps);
	const int64_t trigger = epoch + int64_t(successor) * p + 102'000;
	result out(deadline, trigger);
	std::vector<event> events;
	for (int which = 0; which < 2; ++which)
	{
		events.push_back({epoch, which, 0, 0});
		events.push_back({epoch + 100'000, which, 0, 1});
		events.push_back({epoch + p, which, 1, 0}); // frame 1 end shard is lost
	}
	for (int frame = 2; frame <= successor; ++frame)
	{
		const int64_t at = epoch + int64_t(frame) * p + (frame == successor ? 2'000 : 0);
		events.push_back({at, 1, uint64_t(frame), 0});
		events.push_back({at + 100'000, 1, uint64_t(frame), 1});
	}
	// Repaired left end shard and right NACK-like end shard have independent
	// arrival times. The successor's real last shard is the deadline trigger.
	events.push_back({repair_left, 0, 1, 1});
	events.push_back({repair_right, 1, 1, 1});
	// Release later left frames only after the repair race, keeping frame 1 the
	// newest possible coherent update during the interval under study.
	for (int frame = 2; frame <= successor; ++frame)
	{
		const int64_t at = trigger + 3 * display_period + int64_t(frame - 2) * p;
		events.push_back({at, 0, uint64_t(frame), 0});
		events.push_back({at + 100'000, 0, uint64_t(frame), 1});
	}
	std::stable_sort(events.begin(), events.end(), [](const event & a, const event & b) { return a.at < b.at; });
	for (const event & e: events)
	{
		out.replay.render_through(e.at - 1);
		(e.eye_index == 0 ? out.replay.left : out.replay.right).push(e.frame, e.shard, e.at);
		out.replay.render_through(e.at);
	}
	out.replay.end = trigger + 3 * display_period + int64_t(successor - 2) * p + 5 * display_period;
	out.replay.render_through(out.replay.end);
	out.repair_left_accepted = std::find(out.replay.left.decoded.begin(), out.replay.left.decoded.end(), 1) != out.replay.left.decoded.end();
	out.repair_right_accepted = std::find(out.replay.right.decoded.begin(), out.replay.right.decoded.end(), 1) != out.replay.right.decoded.end();
	return out;
}

void print(int fps, const char * name, const result & r)
{
	std::printf("%2d %-15s L1/R1=%d/%d updates=", fps, name,
	            r.repair_left_accepted, r.repair_right_accepted);
	for (uint64_t id: r.replay.updates) std::printf("%llu,", static_cast<unsigned long long>(id));
	std::printf(" current=%lld holds=%llu retiredL/R=%zu/%zu oldL/R=%llu/%llu\n",
	            static_cast<long long>(r.replay.current.value_or(UINT64_MAX)),
	            static_cast<unsigned long long>(r.replay.held_refreshes),
	            r.replay.left.retired.size(), r.replay.right.retired.size(),
	            static_cast<unsigned long long>(r.replay.left.too_old),
	            static_cast<unsigned long long>(r.replay.right.too_old));
}
} // namespace

int main()
{
	std::puts("source scenario       L1/R1 pair-updates current holds retired(L/R) late(L/R)");
	for (int fps: {30, 60, 90})
	{
		const int64_t p = 1'000'000'000LL / fps;
		const int64_t first = epoch + p;
		const int64_t threshold = first + 2 * display_period;
		const int tf = trigger_frame(fps);
		const int64_t trigger = epoch + int64_t(tf) * p + 102'000;
		const result before_default = run(fps, false, threshold - 1'000, threshold - 1'000);
		const result before_deadline = run(fps, true, threshold - 1'000, threshold - 1'000);
		const result after_default = run(fps, false, threshold + 1'000, threshold + 1'000);
		const result after_deadline = run(fps, true, threshold + 1'000, threshold + 1'000);
		const result cross_default = run(fps, false, trigger - 1'000, trigger + 1'000);
		const result cross_deadline = run(fps, true, trigger - 1'000, trigger + 1'000);
		print(fps, "before/skew3", before_default);
		print(fps, "before/2period", before_deadline);
		print(fps, "after/skew3", after_default);
		print(fps, "after/2period", after_deadline);
		print(fps, "cross/skew3", cross_default);
		print(fps, "cross/2period", cross_deadline);
		std::vector<uint64_t> expected{0};
		for (int frame = 1; frame <= tf; ++frame) expected.push_back(frame);
		std::vector<uint64_t> expected_skipped{0};
		for (int frame = 2; frame <= tf; ++frame) expected_skipped.push_back(frame);
		if (not before_deadline.repair_left_accepted or not before_deadline.repair_right_accepted or
		    not after_deadline.repair_left_accepted or not after_deadline.repair_right_accepted or
		    not cross_deadline.repair_left_accepted or cross_deadline.repair_right_accepted or
		    not cross_default.repair_left_accepted or not cross_default.repair_right_accepted or
		    before_deadline.replay.updates != expected or after_deadline.replay.updates != expected or
		    cross_default.replay.updates != expected or cross_deadline.replay.updates != expected_skipped or
		    cross_deadline.replay.held_refreshes != cross_default.replay.held_refreshes + 1 or
		    cross_deadline.replay.right.too_old != 1 or cross_default.replay.right.too_old != 0)
			return 1;
	}
	return 0;
}
