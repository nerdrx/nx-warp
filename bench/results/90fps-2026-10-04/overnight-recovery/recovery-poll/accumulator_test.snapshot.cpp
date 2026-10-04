// Multipath stage 3: the deepened video reassembly window on the headset.
//
// Both pieces under test are the real ones the decoder uses — wivrn::shard_set
// (client/decoder/shard_set.h) holds one frame's shards, wivrn::frame_window
// (client/decoder/frame_window.h) holds the frames and owns the whole
// advance/discard policy — so this is not a model of the logic, it is the logic.
// What it does not cover is the Vulkan/MediaCodec end of shard_accumulator.
//
// Part A: the window's index arithmetic. Where a frame index lands, what happens
// to a frame older than the window, and what a gap wider than the window does.
// Part B: the skew rule, which is the point of the whole change. A complete frame
// up to `skew` newer must NOT retire the one in front of it — that is the normal
// state of affairs when a frame is split across two links of different latency —
// and one further than that must.
// Part C: draining. Frames reach the decoder in index order and exactly once,
// even when they complete out of order; a frame that is whole but undecodable
// does not wedge the window; the hard depth bound retires the oldest frames when
// nothing ever completes.
// Part D: shard_set. Duplicate shards, which is what the same shard arriving over
// both paths looks like, are dropped; completeness needs every shard plus the end
// marker; the submitted cursor makes a second look at a frame idempotent, which a
// window that revisits a frame after a newer one moved makes reachable.
// Part E: the striping scenario end to end. The Wi-Fi prefix of frame N is still
// arriving while the USB tail of N+1 and N+2 has already landed; nothing may be
// dropped, and everything must reach the decoder in order.
//
// Build:
//   g++ -std=c++23 -I common -I client/decoder -I build-client/common -I external \
//       -I build-client/_deps/boost-src/libs/pfr/include \
//       -o accumulator_test tests/accumulator_test.cpp common/smp.cpp -lcrypto
//   ./accumulator_test

#include "frame_window.h"
#include "nack_deadline.h"
#include "shard_set.h"

#include <cstdio>
#include <string>
#include <vector>

using wivrn::shard_set;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                                   \
	do                                                                            \
	{                                                                             \
		++checks;                                                             \
		if (not(cond))                                                        \
		{                                                                     \
			++failures;                                                   \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                     \
	} while (0)

namespace
{

using data_shard = wivrn::to_headset::video_stream_data_shard;
using window_t = wivrn::frame_window<shard_set, 6, 3>;

// A frame of `n` shards, delivered a shard at a time. The last shard carries the
// timing info, which is what marks the end of a frame on the wire.
data_shard make_shard(uint64_t frame, uint16_t idx, uint16_t count)
{
	data_shard s{};
	s.stream_item_idx = 0;
	s.frame_idx = frame;
	s.shard_idx = idx;
	s.payload = {};
	if (idx == 0)
		s.view_info.emplace();
	if (idx + 1 == count)
		s.timing_info.emplace();
	return s;
}

// The whole of shard_accumulator except the decoder: the same window, the same
// sets, the same drain, with the decoder replaced by a log of what it was fed.
struct harness
{
	window_t window{shard_set(0)};

	// Frame indices handed to the decoder, and the shard runs they were handed in
	std::vector<uint64_t> decoded;
	std::vector<std::pair<uint64_t, std::pair<uint16_t, uint16_t>>> pushes;
	// Frames given up on
	std::vector<uint64_t> retired;
	// Shards refused as older than the window
	std::vector<uint64_t> too_old;

	int64_t now = 1000;

	void push(uint64_t frame, uint16_t idx, uint16_t count)
	{
		shard_set * set = window.slot(frame, [this](shard_set & s) { retired.push_back(s.frame_index()); });
		if (not set)
		{
			too_old.push_back(frame);
			return;
		}

		set->insert(make_shard(frame, idx, count), now++);
		if (set->complete())
			window.note_complete(frame);

		pump();
	}

	// A whole frame at once
	void push_frame(uint64_t frame, uint16_t count)
	{
		for (uint16_t i = 0; i < count; ++i)
			push(frame, i, count);
	}

	void pump()
	{
		window.drain(
		        [this](shard_set & set) { return submit(set); },
		        [this](shard_set & set) { retired.push_back(set.frame_index()); });
	}

	window_t::step submit(shard_set & set)
	{
		using step = window_t::step;
		auto & d = set.data;

		uint16_t first = set.submitted;
		uint16_t last = first;
		while (last < d.size() and d[last])
			++last;

		const bool complete = last == d.size() and not d.empty() and d.back()->timing_info;

		if (last > first)
		{
			if (complete and not d.front()->view_info)
				return step::unusable;
			pushes.push_back({set.frame_index(), {first, last}});
			set.submitted = last;
		}

		if (not complete)
			return step::wait;

		if (not d.front()->view_info)
			return step::unusable;

		decoded.push_back(set.frame_index());
		return step::done;
	}
};

// --- Part A ---------------------------------------------------------------

void part_a()
{
	std::printf("Part A: the window's index arithmetic\n");

	window_t w{shard_set(7)};
	CHECK(window_t::depth == 6);
	CHECK(window_t::skew == 3);
	CHECK(w.front_index() == 0);
	CHECK(w.front().frame_index() == 0);
	// The stream index reaches every slot
	CHECK(w.front().feedback.stream_index == 7);

	auto none = [](shard_set &) {};

	for (uint64_t i = 0; i < window_t::depth; ++i)
	{
		shard_set * s = w.slot(i, none);
		CHECK(s != nullptr);
		CHECK(s->frame_index() == i);
	}

	// One past the end pushes the oldest out, exactly one of them
	{
		std::vector<uint64_t> gone;
		shard_set * s = w.slot(window_t::depth, [&](shard_set & x) { gone.push_back(x.frame_index()); });
		CHECK(gone.size() == 1);
		CHECK(gone.empty() or gone[0] == 0);
		CHECK(s and s->frame_index() == window_t::depth);
		CHECK(w.front_index() == 1);
	}

	// Anything older than the window is refused outright
	CHECK(w.slot(0, none) == nullptr);

	// advance() frees the oldest slot by handing it the newest index
	{
		const uint64_t front = w.front_index();
		w.advance();
		CHECK(w.front_index() == front + 1);
		shard_set * s = w.slot(front + window_t::depth, none);
		CHECK(s and s->frame_index() == front + window_t::depth);
	}

	// A gap wider than the window restarts it rather than walking the gap. The
	// quad layer stream, silent whenever no layer is promoted, comes back like
	// this after any number of frames.
	{
		window_t g{shard_set(1)};
		std::vector<uint64_t> gone;
		shard_set * s = g.slot(100'000, [&](shard_set & x) { gone.push_back(x.frame_index()); });
		CHECK(gone.size() == window_t::depth);
		CHECK(s and s->frame_index() == 100'000);
		CHECK(g.front_index() == 100'000);
		CHECK(g.front().feedback.stream_index == 1);
		// ... and it is a clean window again, not one that keeps restarting
		shard_set * t = g.slot(100'005, [](shard_set &) {});
		CHECK(t and t->frame_index() == 100'005);
		CHECK(g.front_index() == 100'000);
	}
}

// --- Part B ---------------------------------------------------------------

void part_b()
{
	std::printf("Part B: the skew rule\n");

	// Frame 0 never completes. Frames 1..3 do. None of them is more than `skew`
	// newer than 0, so 0 is still being waited for — this is exactly the state a
	// frame split across two links spends its life in, and the old two-deep
	// window threw the frame away at the first of these.
	{
		harness h;
		h.push(0, 0, 4); // frame 0 starts and stalls
		for (uint64_t f = 1; f <= window_t::skew; ++f)
			h.push_frame(f, 2);

		CHECK(h.retired.empty());
		CHECK(h.decoded.empty());
		CHECK(h.window.front_index() == 0);
		CHECK(h.window.front_stale() == false);

		// One further out and frame 0 is given up on — and then everything that
		// was waiting behind it goes to the decoder at once, in order.
		h.push_frame(window_t::skew + 1, 2);
		CHECK(h.retired.size() == 1);
		CHECK(h.retired.empty() or h.retired[0] == 0);
		CHECK(h.decoded.size() == window_t::skew + 1);
		for (size_t i = 0; i < h.decoded.size(); ++i)
			CHECK(h.decoded[i] == i + 1);
	}

	// An *incomplete* newer frame is no evidence that anything is late, however
	// far ahead it is: it says the link is losing shards, not that the front
	// frame's are never coming.
	{
		harness h;
		h.push(0, 0, 4);
		for (uint64_t f = 1; f < window_t::depth; ++f)
			h.push(f, 0, 4); // started, never completed

		CHECK(h.retired.empty());
		CHECK(h.decoded.empty());
		CHECK(h.window.front_index() == 0);
	}

	// The frame that finally arrives is still accepted: waiting for it was the
	// whole point.
	{
		harness h;
		h.push(0, 0, 2);
		h.push_frame(1, 2);
		h.push_frame(2, 2);
		CHECK(h.decoded.empty());

		h.push(0, 1, 2); // the shard the other path was slow with
		CHECK(h.retired.empty());
		CHECK(h.decoded.size() == 3);
		CHECK(h.decoded.size() == 3 and h.decoded[0] == 0 and h.decoded[1] == 1 and h.decoded[2] == 2);
	}
}

// --- Part C ---------------------------------------------------------------

void part_c()
{
	std::printf("Part C: draining\n");

	// The straightforward case is unchanged: frames complete in order and go
	// straight through, one push each.
	{
		harness h;
		for (uint64_t f = 0; f < 10; ++f)
			h.push_frame(f, 3);

		CHECK(h.decoded.size() == 10);
		CHECK(h.retired.empty());
		for (size_t i = 0; i < h.decoded.size(); ++i)
			CHECK(h.decoded[i] == i);
	}

	// Out of order completion still reaches the decoder in order, and each frame
	// exactly once: a decoder cannot be fed out of order.
	{
		harness h;
		h.push_frame(2, 2);
		h.push_frame(1, 2);
		CHECK(h.decoded.empty());
		h.push_frame(0, 2);
		CHECK(h.decoded.size() == 3);
		CHECK(h.decoded.size() == 3 and h.decoded[0] == 0 and h.decoded[1] == 1 and h.decoded[2] == 2);
	}

	// Nothing is ever handed to the decoder twice, even though a frame at the
	// head of the window is looked at again on every arrival.
	{
		harness h;
		// A frame delivered a shard at a time, each arrival re-examining it
		h.push(0, 0, 5);
		h.push(0, 1, 5);
		h.push(0, 3, 5); // a hole at 2
		h.push(0, 2, 5); // fills it: 2 and 3 go over together
		h.push(0, 4, 5);

		size_t covered = 0;
		for (const auto & [frame, run]: h.pushes)
		{
			CHECK(frame == 0);
			CHECK(run.first == covered);
			covered = run.second;
		}
		CHECK(covered == 5);
		CHECK(h.decoded.size() == 1);
	}

	// The hard bound: nothing ever completes, so the skew rule never fires, and
	// the window's own depth is what retires the oldest frames. At most `depth`
	// frames' shards are held at once, which is the memory bound.
	{
		harness h;
		for (uint64_t f = 0; f < 20; ++f)
			h.push(f, 0, 4);

		CHECK(h.decoded.empty());
		CHECK(h.retired.size() == 20 - window_t::depth);
		for (size_t i = 0; i < h.retired.size(); ++i)
			CHECK(h.retired[i] == i);
		// The window holds the newest `depth` frames and nothing else
		CHECK(h.window.front_index() == 20 - window_t::depth);
	}

	// A shard for a frame that has already left the window is dropped, not filed
	// under some other frame. Two paths make this reachable in a way one never did.
	{
		harness h;
		for (uint64_t f = 0; f < 10; ++f)
			h.push_frame(f, 2);
		h.push(3, 0, 2);
		CHECK(h.too_old.size() == 1);
		CHECK(h.too_old.empty() or h.too_old[0] == 3);
	}

	// A frame that is whole but has no view_info on its first shard cannot be
	// decoded at all. It must be given up on rather than left blocking the window.
	{
		harness h;
		shard_set * s = h.window.slot(0, [](shard_set &) {});
		data_shard first = make_shard(0, 0, 2);
		first.view_info.reset();
		s->insert(std::move(first), 1);
		s->insert(make_shard(0, 1, 2), 2);
		h.window.note_complete(0);
		h.pump();

		CHECK(h.decoded.empty());
		CHECK(h.retired.size() == 1);
		CHECK(h.window.front_index() == 1);
	}
}

// --- Part D ---------------------------------------------------------------

void part_d()
{
	std::printf("Part D: one frame's shards\n");

	shard_set s(2);
	CHECK(s.feedback.stream_index == 2);
	CHECK(s.empty());
	CHECK(not s.complete());

	CHECK(s.insert(make_shard(0, 0, 3), 100).value_or(-1) == 0);
	CHECK(not s.empty());
	CHECK(s.feedback.received_first_packet == 100);

	// The same shard again — which is what one arriving over each path would look
	// like — changes nothing at all.
	CHECK(not s.insert(make_shard(0, 0, 3), 200).has_value());
	CHECK(s.data.size() == 1);
	CHECK(s.feedback.received_first_packet == 100);

	// Out of order insertion grows the frame without losing the hole in between
	CHECK(s.insert(make_shard(0, 2, 3), 300).value_or(-1) == 2);
	CHECK(s.data.size() == 3);
	CHECK(not s.data[1].has_value());
	CHECK(not s.complete());

	CHECK(s.insert(make_shard(0, 1, 3), 400).value_or(-1) == 1);
	CHECK(s.complete());

	// Missing the end marker is not complete however many shards are there
	{
		shard_set t(0);
		t.insert(make_shard(0, 0, 3), 1);
		t.insert(make_shard(0, 1, 3), 2);
		CHECK(not t.complete());
	}

	// reset clears everything, the submitted cursor included: a slot that comes
	// back round must not think it has already fed the decoder.
	s.submitted = 3;
	s.reset(11);
	CHECK(s.frame_index() == 11);
	CHECK(s.feedback.stream_index == 2);
	CHECK(s.submitted == 0);
	CHECK(s.empty());
	CHECK(s.parity.empty());
	CHECK(s.feedback.received_first_packet == 0);
}

// --- Part E ---------------------------------------------------------------

void part_e()
{
	std::printf("Part E: striping, both paths in flight\n");

	// Three frames of eight shards each. The first five shards of every frame go
	// over Wi-Fi, paced, and the last three go over USB immediately — so the USB
	// tail of a frame arrives before the Wi-Fi middle of it, and the tail of frame
	// N+1 can arrive before any of frame N's Wi-Fi shards have.
	//
	// Nothing here may be dropped and everything must reach the decoder in order.
	const uint16_t count = 8;
	const uint16_t split = 5;

	harness h;

	// Every frame's USB tail lands first, all three frames' worth
	for (uint64_t f = 0; f < 3; ++f)
		for (uint16_t i = split; i < count; ++i)
			h.push(f, i, count);

	CHECK(h.retired.empty());
	CHECK(h.decoded.empty());
	CHECK(h.window.front_index() == 0);

	// ... then the Wi-Fi prefixes trickle in, in frame order
	for (uint64_t f = 0; f < 3; ++f)
		for (uint16_t i = 0; i < split; ++i)
			h.push(f, i, count);

	CHECK(h.retired.empty());
	CHECK(h.too_old.empty());
	CHECK(h.decoded.size() == 3);
	CHECK(h.decoded.size() == 3 and h.decoded[0] == 0 and h.decoded[1] == 1 and h.decoded[2] == 2);

	// Every shard reached the decoder exactly once, in order, per frame
	std::vector<uint16_t> covered(3, 0);
	for (const auto & [frame, run]: h.pushes)
	{
		CHECK(frame < 3);
		if (frame < 3)
		{
			CHECK(run.first == covered[frame]);
			covered[frame] = run.second;
		}
	}
	for (uint64_t f = 0; f < 3; ++f)
		CHECK(covered[f] == count);

	// The worst case the window is sized for: the USB path runs a full `skew`
	// frames ahead of the Wi-Fi one, forever. Nothing is ever dropped.
	{
		harness g;
		const uint64_t frames = 40;
		for (uint64_t f = 0; f < frames + window_t::skew; ++f)
		{
			if (f < frames)
				for (uint16_t i = split; i < count; ++i)
					g.push(f, i, count);
			if (f >= window_t::skew)
				for (uint16_t i = 0; i < split; ++i)
					g.push(f - window_t::skew, i, count);
		}

		CHECK(g.retired.empty());
		CHECK(g.too_old.empty());
		CHECK(g.decoded.size() == frames);
	}
}

// The production window can opt into a shorter ASTC tolerance. This checks
// retirement order and late-shard refusal; it does not exercise Vulkan or XR.
void part_f()
{
	std::printf("Part F: opt-in independent-codec tolerance\n");
	for (uint64_t tolerance: {uint64_t(0), uint64_t(1), uint64_t(3)})
	{
		window_t window{shard_set(0), tolerance};
		std::vector<uint64_t> retired, delivered;
		auto visit = [&](shard_set & set) {
			if (!set.complete()) return window_t::step::wait;
			delivered.push_back(set.frame_index());
			return window_t::step::done;
		};
		auto retire = [&](shard_set & set) { retired.push_back(set.frame_index()); };
		window.slot(0, retire)->insert(make_shard(0, 0, 2), 1000);
		// Complete newer frames must wait only through the chosen tolerance.
		for (uint64_t index = 1; index <= tolerance; ++index)
		{
			window.slot(index, retire)->insert(make_shard(index, 0, 1), 1000 + index);
			window.note_complete(index);
			window.drain(visit, retire);
			CHECK(retired.empty());
			CHECK(delivered.empty());
		}
		const uint64_t newest = tolerance + 1;
		window.slot(newest, retire)->insert(make_shard(newest, 0, 1), 1000 + newest);
		window.note_complete(newest);
		window.drain(visit, retire);
		CHECK(retired == std::vector<uint64_t>{0});
		CHECK(delivered.size() == newest);
		for (size_t i = 0; i < delivered.size(); ++i) CHECK(delivered[i] == i + 1);
		CHECK(window.slot(0, retire) == nullptr);
		CHECK(window.front_index() == newest + 1);
	}
	// Without any complete successor, a zero-skew window still waits for repair.
	window_t window{shard_set(0), 0};
	window.slot(0, [](auto &) {})->insert(make_shard(0, 0, 2), 1000);
	window.drain([](auto &) { return window_t::step::wait; }, [](auto &) { CHECK(false); });
	CHECK(window.front_index() == 0);
}

void part_g()
{
	std::printf("Part G: arrival-driven ASTC deadline\n");
	constexpr int64_t period = 10;
	auto run = [](bool deadline_enabled, int64_t now, int64_t display_period, bool complete_successor,
	              bool complete_front = false, bool incomplete_successor = false) {
		window_t w{shard_set(0)};
		auto retire = [](shard_set &) {};
		w.slot(0, retire)->insert(make_shard(0, 0, 2), 100);
		if (complete_successor)
		{
			w.slot(1, retire)->insert(make_shard(1, 0, 1), 101);
			w.note_complete(1);
		}
		else if (incomplete_successor)
			w.slot(1, retire)->insert(make_shard(1, 0, 2), 101);
		if (complete_front)
		{
			w.front().insert(make_shard(0, 1, 2), 102);
			w.note_complete(0);
		}
		std::vector<uint64_t> retired;
		w.drain(
		        [](shard_set & s) { return s.complete() ? window_t::step::done : window_t::step::wait; },
		        [&](shard_set & s) { retired.push_back(s.frame_index()); },
		        [&](const shard_set & s) {
			        return wivrn::frame_deadline_expired(now, s.feedback.received_first_packet,
			                                             display_period, deadline_enabled,
			                                             w.has_newer_complete_than_front());
		        });
		return std::pair{w.front_index(), retired};
	};

	CHECK(run(true, 119, period, true).first == 0); // one tick before threshold
	CHECK(run(true, 120, period, true).second == std::vector<uint64_t>{0});
	CHECK(run(false, 120, period, true).first == 0); // default off
	CHECK(run(true, 120, period, false).first == 0); // no complete successor
	CHECK(run(true, 120, period, false, false, true).first == 0); // incomplete successor
	CHECK(run(true, 120, period, true, true).first == 2); // complete front and successor submit
	CHECK(not wivrn::frame_deadline_expired(120, 0, period, true, true)); // unknown first receive
	CHECK(not wivrn::frame_deadline_expired(120, 100, 0, true, true)); // unknown period
	CHECK(not wivrn::frame_deadline_expired(99, 100, period, true, true)); // clock rollback
	CHECK(not wivrn::frame_deadline_expired(INT64_MAX, 1, INT64_MAX, true, true)); // multiply overflow

	// Trickling shards do not restart the timer: shard_set retains first arrival.
	shard_set trickle(0);
	trickle.insert(make_shard(0, 0, 3), 100);
	trickle.insert(make_shard(0, 1, 3), 119);
	CHECK(trickle.feedback.received_first_packet == 100);
	CHECK(wivrn::frame_deadline_expired(120, trickle.feedback.received_first_packet,
	                                   period, true, true));
}

void part_h()
{
	constexpr int64_t quiet = wivrn::nack_quiet_period_ns;
	shard_set holes(0);
	holes.insert(make_shard(0, 0, 4), 10'000'000);
	holes.insert(make_shard(0, 2, 4), 10'000'000);
	std::vector<uint16_t> missing;
	int scans = 0;
	auto due = [&](int64_t now, bool enabled = true, bool send_available = true) {
		return wivrn::nack_poll_deadline(now, holes.last_shard, holes.nack_last, holes.nack_rounds, enabled, send_available, not holes.empty(), holes.complete(), [&]() {
			++scans;
			if (holes.complete())
				return false;
			holes.missing_shards(missing, false);
			return not missing.empty();
		});
	};
	const int64_t first_due = 10'000'000 + quiet;
	CHECK(due(first_due - 1) == first_due);
	CHECK(scans == 0); // no hole scan before due
	CHECK(due(first_due) == first_due);
	CHECK(missing == std::vector<uint16_t>{1});
	CHECK(wivrn::nack_poll_timeout(first_due - 1, due(first_due - 1), std::chrono::milliseconds(100)).count() == 1);
	CHECK(wivrn::nack_poll_timeout(1, 1'000'000'001, std::chrono::milliseconds(200)).count() == 100);
	holes.nack_rounds = 1;
	holes.nack_last = first_due;
	CHECK(due(first_due + quiet) == first_due + quiet);
	holes.nack_rounds = 2;
	CHECK(not due(first_due + quiet * 2));
	CHECK(wivrn::nack_poll_timeout(first_due + quiet * 2, due(first_due + quiet * 2), std::chrono::milliseconds(100)).count() == 100);

	// An arrival drained immediately before poll restarts the quiet gate.
	holes.nack_rounds = 0;
	holes.insert(make_shard(0, 3, 4), first_due + quiet + 1);
	CHECK(holes.last_shard == first_due + quiet + 1);
	auto fresh_due = due(first_due + quiet * 2);
	CHECK(fresh_due == std::optional<int64_t>(holes.last_shard + quiet));
	CHECK(due(holes.last_shard + quiet) == std::optional<int64_t>(holes.last_shard + quiet));

	// A contiguous prefix without an end marker has no actionable inferred tail.
	shard_set unknown_tail(0);
	unknown_tail.insert(make_shard(1, 0, 3), 20'000'000);
	unknown_tail.insert(make_shard(1, 1, 3), 20'000'000);
	int unknown_scans = 0;
	auto unknown_due = [&](int64_t now) {
		return wivrn::nack_poll_deadline(now, unknown_tail.last_shard, unknown_tail.nack_last, 0, true, true, true, false, [&]() {
			++unknown_scans;
			std::vector<uint16_t> holes;
			unknown_tail.missing_shards(holes, false);
			return not holes.empty();
		});
	};
	CHECK(unknown_due(20'000'000 + quiet - 1) == 20'000'000 + quiet);
	CHECK(not unknown_due(20'000'000 + quiet));
	CHECK(unknown_scans == 1);
	CHECK(wivrn::nack_poll_timeout(20'000'000 + quiet, unknown_due(20'000'000 + quiet), std::chrono::milliseconds(100)).count() == 100);

	// If parity can fill the sole hole, it is not actionable for NACK.
	shard_set parity(0);
	parity.insert(make_shard(2, 0, 3), 30'000'000);
	parity.insert(make_shard(2, 2, 3), 30'000'000);
	wivrn::to_headset::video_stream_parity_shard p{};
	p.first_shard_idx = 0;
	p.shard_stride = 1;
	p.blob_size = {1, 1, 1};
	parity.parity.push_back(p);
	std::vector<uint16_t> parity_holes;
	CHECK(wivrn::nack_poll_deadline(30'000'000 + quiet, parity.last_shard, 0, 0, true, true, true, false, [&]() {
		      parity.missing_shards(parity_holes, false);
		      return not parity_holes.empty();
	      }) == std::nullopt);
	CHECK(parity_holes.empty());
	CHECK(not due(first_due + quiet * 2 + 1, false, true));     // disabled
	CHECK(not due(first_due + quiet * 2 + 1, true, false));     // unavailable sender/weak scene
	CHECK(not wivrn::nack_quiet_elapsed(10, 11));               // rollback
	CHECK(not wivrn::nack_quiet_elapsed(INT64_MAX, INT64_MAX)); // overflow-safe
	CHECK(not wivrn::nack_poll_deadline(INT64_MAX, INT64_MAX, 0, 0, true, true, true, false, [] { return true; }));
	CHECK(not wivrn::nack_poll_deadline(9, 10, 0, 0, true, true, true, false, [] { return true; })); // clock rollback
	shard_set whole(0);
	whole.insert(make_shard(3, 0, 1), 40'000'000);
	CHECK(not wivrn::nack_poll_deadline(40'000'000 + quiet - 1, whole.last_shard, 0, 0, true, true, true, whole.complete(), [] { return true; }));
}

} // namespace

int main()
{
	std::printf("Video reassembly window test\n\n");
	part_a();
	part_b();
	part_c();
	part_d();
	part_e();
	part_f();
	part_g();
	part_h();
	std::printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
