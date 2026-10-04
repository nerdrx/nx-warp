// Shard retransmission: the two halves of it that can be driven without a socket.
//
// Part A: the server's send history (server/encoder/shard_history.h). The ring is
// byte bounded, so what it holds is bounded whatever the bitrate; a shard that has
// aged out has to be *gone* rather than half overwritten; a shard that spilled to the
// secondary (TCP) path must never be in there at all, since answering a request for
// one would spend Wi-Fi bandwidth on a shard that was never at risk; and the blobs
// that come back out have to be the blobs that went in, byte for byte, because a
// retransmission is a fec::decode_blob of one.
// Part B: serving a request. The bitmap the headset sends names shards; what comes
// back must be exactly those of them the ring still has, in index order, capped, and
// a request for a frame the ring never saw must come back empty rather than wrong.
// Part C: the headset's gap detection (client/decoder/shard_set.h). Which holes are
// worth asking about — the ones parity cannot repair on its own, and only those —
// including the group that is two shards short, where all but one of them is asked
// for because filling those is what puts the last back in the parity's reach.
// Part D: the rate limit. One request per frame per round, at most two rounds, and
// the round only opens once the frame has been quiet for the delay; a frame that
// stays broken must cost a fixed, small number of requests and then stop.
// Part E: the bitmap itself. Building it from a list of indices and reading it back
// has to round trip, has to stay inside max_nack_bitmap_bytes, and a request whose
// holes run wider than that window must be truncated rather than dropped.
//
// Build (the boost include is wherever the build fetched it):
//   g++ -std=c++23 -I common -I client/decoder -I server/encoder -I build-client/common \
//       -I external -I build-client/_deps/boost-src/libs/pfr/include \
//       -o nack_test tests/nack_test.cpp common/smp.cpp -lcrypto
//   ./nack_test

#include "fec.h"
#include "shard_history.h"
#include "shard_set.h"
#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

using namespace wivrn;

static int failures = 0;
static int checks = 0;

#define CHECK(...)                                                                           \
	do                                                                                   \
	{                                                                                    \
		++checks;                                                                    \
		if (not(__VA_ARGS__))                                                        \
		{                                                                            \
			++failures;                                                          \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
		}                                                                            \
	} while (0)

namespace
{

using data_shard = to_headset::video_stream_data_shard;
using parity_shard = to_headset::video_stream_parity_shard;
using timing_info_t = data_shard::timing_info_t;
using view_info_t = data_shard::view_info_t;

view_info_t make_view_info()
{
	return view_info_t{
	        .display_time = 42,
	        .pose = {XrPosef{{0, 0, 0, 1}, {0.03f, 0, 0}}, XrPosef{{0, 0, 0, 1}, {-0.03f, 0, 0}}},
	        .fov = {XrFovf{-0.9f, 0.9f, 0.9f, -0.9f}, XrFovf{-0.9f, 0.9f, 0.9f, -0.9f}},
	        .foveation = {to_headset::foveation_parameter{{1, 4, 5, 3, 1}, {2, 3, 4, 3, 2}},
	                      to_headset::foveation_parameter{{1, 4, 5, 3, 1}, {2, 3, 4, 3, 2}}},
	        .alpha = false,
	        .quad = {},
	};
}

// One frame's shards, sharded and grouped the way video_encoder::SendData does it.
struct frame
{
	std::vector<uint8_t> encoded;
	std::vector<data_shard> shards;
	std::vector<parity_shard> parity;
	std::vector<std::vector<uint8_t>> parity_payloads;
};

frame make_frame(size_t shard_count,
                 uint64_t frame_idx = 7,
                 uint16_t k = fec::group_size,
                 uint16_t depth = 1)
{
	const size_t budget = fec::shard_payload_budget(true, k) - 200;

	frame f;
	f.encoded.resize(shard_count * budget);
	for (size_t i = 0; i < f.encoded.size(); ++i)
		f.encoded[i] = uint8_t(i * 13 + (i >> 7) * 5 + 1);

	fec::group_builder builder;
	builder.set_layout(k, depth);
	builder.reset(0, frame_idx);

	auto drain = [&] {
		while (auto p = builder.take())
		{
			f.parity_payloads.emplace_back(p->payload.begin(), p->payload.end());
			p->payload = f.parity_payloads.back();
			f.parity.push_back(std::move(*p));
		}
	};

	data_shard shard;
	shard.stream_item_idx = 0;
	shard.frame_idx = frame_idx;
	shard.view_info = make_view_info();

	for (size_t i = 0; i < shard_count; ++i)
	{
		shard.shard_idx = uint16_t(i);
		if (i + 1 == shard_count)
			shard.timing_info = timing_info_t{1, 2, 3, 4};
		shard.payload = std::span<uint8_t>(f.encoded).subspan(i * budget, budget);

		f.shards.push_back(shard);
		builder.add(shard);
		if (builder.block_full())
			drain();

		shard.view_info.reset();
	}
	drain();

	auto moved = std::move(f);
	for (size_t i = 0; i < moved.shards.size(); ++i)
		moved.shards[i].payload = std::span<uint8_t>(moved.encoded).subspan(i * budget, budget);
	for (size_t i = 0; i < moved.parity.size(); ++i)
		moved.parity[i].payload = moved.parity_payloads[i];
	return moved;
}

// The bitmap the headset builds, and the reader the server applies to it. Kept here in
// one place because both ends have to agree on it exactly.
from_headset::nack make_request(uint8_t stream, uint64_t frame_idx, std::span<const uint16_t> missing)
{
	from_headset::nack n{
	        .stream_index = stream,
	        .frame_idx = frame_idx,
	        .first_shard_idx = missing.empty() ? uint16_t(0) : missing.front(),
	        .bitmap = {},
	};
	for (uint16_t idx: missing)
	{
		const size_t bit = size_t(idx) - n.first_shard_idx;
		const size_t byte = bit / 8;
		if (byte >= from_headset::max_nack_bitmap_bytes)
			break;
		if (n.bitmap.size() <= byte)
			n.bitmap.resize(byte + 1, 0);
		n.bitmap[byte] |= uint8_t(1u << (bit % 8));
	}
	return n;
}

std::vector<uint16_t> read_request(const from_headset::nack & n)
{
	std::vector<uint16_t> out;
	for (size_t byte = 0; byte < n.bitmap.size(); ++byte)
		for (size_t bit = 0; bit < 8; ++bit)
			if (n.bitmap[byte] & (1u << bit))
				out.push_back(uint16_t(n.first_shard_idx + byte * 8 + bit));
	return out;
}

// Fill the history exactly as SendData does: the recovery blob of every shard.
void send_frame(shard_history & h, const frame & f, bool on_primary = true)
{
	std::vector<uint8_t> blob;
	for (const data_shard & s: f.shards)
	{
		fec::encode_blob(s, blob);
		h.push(s.frame_idx, s.shard_idx, blob, on_primary);
	}
	h.end_frame(f.shards.front().frame_idx, uint32_t(f.shards.size()));
}

bool same_shard(const data_shard & a, const data_shard & b)
{
	if (a.stream_item_idx != b.stream_item_idx or a.frame_idx != b.frame_idx or a.shard_idx != b.shard_idx)
		return false;
	if (a.payload.size() != b.payload.size())
		return false;
	if (std::memcmp(a.payload.data(), b.payload.data(), a.payload.size()) != 0)
		return false;
	if (a.view_info.has_value() != b.view_info.has_value())
		return false;
	if (a.timing_info.has_value() != b.timing_info.has_value())
		return false;
	if (a.view_info and a.view_info->display_time != b.view_info->display_time)
		return false;
	if (a.timing_info and a.timing_info->send_end != b.timing_info->send_end)
		return false;
	return true;
}

void test_history_ring()
{
	std::printf("Part A: the send history\n");

	// Off is off: no ring, no memory, nothing served
	{
		shard_history h;
		CHECK(not h.enabled());
		CHECK(h.bytes() == 0);
		frame f = make_frame(20);
		send_frame(h, f);
		CHECK(h.held() == 0);
		CHECK(h.bytes() == 0);
		// The frame counts are kept either way — the adaptive parity ratio needs
		// them whether or not anything is ever asked for again
		auto cost = h.frame_cost(7);
		CHECK(cost.has_value());
		CHECK(cost and cost->shards_sent == 20);
	}

	shard_history h;
	h.set_enabled(true);
	CHECK(h.enabled());
	CHECK(h.bytes() == shard_history::capacity);

	frame f = make_frame(40);
	send_frame(h, f);
	CHECK(h.held() == 40);

	// What comes out is what went in, byte for byte, and it turns back into the shard
	// through the same decode_blob a parity reconstruction uses
	{
		std::vector<uint16_t> want;
		for (uint16_t i = 0; i < 40; ++i)
			want.push_back(i);
		std::vector<shard_history::hit> hits;
		CHECK(h.collect(7, 0, make_request(0, 7, want).bitmap, 64, hits) == 40);
		CHECK(hits.size() == 40);
		for (size_t i = 0; i < hits.size(); ++i)
		{
			CHECK(hits[i].shard_idx == i);
			auto rebuilt = fec::decode_blob(0, 7, hits[i].shard_idx, hits[i].blob);
			CHECK(same_shard(rebuilt, f.shards[i]));
		}
		// The first shard's pose and the last one's timings survive the round trip,
		// which is what makes a retransmission able to finish a frame at all
		CHECK(fec::decode_blob(0, 7, 0, hits.front().blob).view_info.has_value());
		CHECK(fec::decode_blob(0, 7, 39, hits.back().blob).timing_info.has_value());
	}

	// Shards that spilled to the secondary path are not kept: TCP does not lose one,
	// so a request naming it is the two paths arriving out of order and a duplicate
	// over Wi-Fi would help nothing
	{
		shard_history spill;
		spill.set_enabled(true);
		frame g = make_frame(10, 9);
		send_frame(spill, g, false);
		CHECK(spill.held() == 0);
		std::vector<shard_history::hit> hits;
		CHECK(spill.collect(9, 0, make_request(0, 9, std::vector<uint16_t>{0, 1, 2}).bitmap, 64, hits) == 0);
		// The frame length is still recorded: it is not a retransmission fact
		CHECK(spill.frame_cost(9).has_value());
	}

	// The ring is byte bounded. Push far more than it can hold and it must hold a
	// bounded amount, and everything it *does* hold must still decode — a half
	// overwritten blob served as a shard is the failure mode this bound exists for.
	{
		shard_history big;
		big.set_enabled(true);
		size_t sent = 0;
		for (uint64_t frame_idx = 0; frame_idx < 60; ++frame_idx)
		{
			frame g = make_frame(40, frame_idx);
			send_frame(big, g);
			sent += g.shards.size();
		}
		CHECK(sent > 2000);
		CHECK(big.held() <= shard_history::max_entries);
		CHECK(big.held() * 1000 < shard_history::capacity + 1000);
		CHECK(big.bytes() == shard_history::capacity);

		// The newest frame is definitely still there and definitely still decodes
		frame newest = make_frame(40, 60);
		send_frame(big, newest);
		std::vector<uint16_t> want = {0, 5, 39};
		std::vector<shard_history::hit> hits;
		CHECK(big.collect(60, 0, make_request(0, 60, want).bitmap, 64, hits) == 3);
		for (const shard_history::hit & hit: hits)
		{
			auto rebuilt = fec::decode_blob(0, 60, hit.shard_idx, hit.blob);
			CHECK(same_shard(rebuilt, newest.shards[hit.shard_idx]));
		}

		// And the oldest is gone rather than corrupt: nothing is served for it
		std::vector<shard_history::hit> stale;
		CHECK(big.collect(0, 0, make_request(0, 0, want).bitmap, 64, stale) == 0);
		CHECK(stale.empty());
	}

	// Turning it off releases the memory and forgets everything
	h.set_enabled(false);
	CHECK(h.bytes() == 0);
	CHECK(h.held() == 0);
	std::vector<shard_history::hit> hits;
	CHECK(h.collect(7, 0, make_request(0, 7, std::vector<uint16_t>{1}).bitmap, 64, hits) == 0);

	std::printf("  ring bounded at %zu kB, %zu entries max\n",
	            shard_history::capacity / 1024,
	            shard_history::max_entries);
}

void test_serving_a_request()
{
	std::printf("Part B: answering one request\n");

	shard_history h;
	h.set_enabled(true);
	frame f = make_frame(30);
	send_frame(h, f);

	// Exactly the shards named, in index order, and nothing else
	{
		std::vector<uint16_t> want = {3, 4, 11, 29};
		std::vector<shard_history::hit> hits;
		CHECK(h.collect(7, want.front(), make_request(0, 7, want).bitmap, 64, hits) == want.size());
		CHECK(hits.size() == want.size());
		for (size_t i = 0; i < hits.size(); ++i)
			CHECK(hits[i].shard_idx == want[i]);
	}

	// A frame the history never saw: empty, not wrong
	{
		std::vector<shard_history::hit> hits;
		CHECK(h.collect(1234, 0, make_request(0, 1234, std::vector<uint16_t>{0, 1}).bitmap, 64, hits) == 0);
	}

	// Shard indices that were never sent: the ones that were come back, the rest are
	// simply not there. Guessing at the shard past the end of a frame is how the
	// headset asks for a lost last shard, and it must cost nothing when it guesses
	// wrong.
	{
		std::vector<uint16_t> want = {28, 29, 30, 31};
		std::vector<shard_history::hit> hits;
		CHECK(h.collect(7, want.front(), make_request(0, 7, want).bitmap, 64, hits) == 2);
		CHECK(hits.size() == 2);
		CHECK(hits[0].shard_idx == 28);
		CHECK(hits[1].shard_idx == 29);
	}

	// The cap is honoured: a request naming everything is answered with at most the
	// budget, so one request can never turn into a frame's worth of extra traffic
	{
		std::vector<uint16_t> want;
		for (uint16_t i = 0; i < 30; ++i)
			want.push_back(i);
		std::vector<shard_history::hit> hits;
		CHECK(h.collect(7, 0, make_request(0, 7, want).bitmap, 8, hits) == 8);
		CHECK(hits.size() == 8);
	}

	// An empty bitmap asks for nothing
	{
		std::vector<shard_history::hit> hits;
		CHECK(h.collect(7, 0, {}, 64, hits) == 0);
	}

	// A first_shard_idx past everything held names nothing
	{
		std::vector<shard_history::hit> hits;
		CHECK(h.collect(7, 200, make_request(0, 7, std::vector<uint16_t>{200, 201}).bitmap, 64, hits) == 0);
	}

	// The nacked count folds into the frame's cost, which is what the adaptive ratio
	// reads as loss the parity did not absorb
	{
		CHECK(h.frame_cost(7)->shards_nacked == 0);
		h.note_nacked(7, 4);
		h.note_nacked(7, 2);
		CHECK(h.frame_cost(7)->shards_sent == 30);
		CHECK(h.frame_cost(7)->shards_nacked == 6);
		// A frame that is no longer tracked says nothing rather than lying
		h.note_nacked(999, 5);
		CHECK(not h.frame_cost(999).has_value());
	}
}

// Build a shard_set holding `f` minus `dropped`, with the parity shards of the groups
// that still have a hole — which is what the accumulator keeps, since a parity for a
// whole group is dropped on arrival.
shard_set receive(const frame & f, std::span<const size_t> dropped, bool keep_parity)
{
	shard_set set;
	set.reset(f.shards.front().frame_idx);

	for (size_t i = 0; i < f.shards.size(); ++i)
	{
		bool gone = false;
		for (size_t d: dropped)
			gone = gone or d == i;
		if (not gone)
			set.insert(data_shard(f.shards[i]), 1000 + int64_t(i));
	}

	if (keep_parity)
	{
		for (const parity_shard & p: f.parity)
		{
			if (set.group_complete(p))
				continue;
			set.parity.push_back(p);
			// The accumulator drains straight away: a group one short of whole
			// repairs itself and its parity is spent.
			set.reconstruct(set.parity.back(), 2000);
		}
		// Drop the spent ones, as drain_parity does
		std::vector<parity_shard> kept;
		for (parity_shard & p: set.parity)
			if (not set.group_complete(p))
				kept.push_back(std::move(p));
		set.parity = std::move(kept);
	}

	return set;
}

void test_gap_detection()
{
	std::printf("Part C: which holes are worth asking about\n");

	frame f = make_frame(24, 7, fec::group_size, 1);
	std::vector<uint16_t> missing;

	// A whole frame asks for nothing
	{
		shard_set set = receive(f, {}, true);
		set.missing_shards(missing, true);
		CHECK(missing.empty());
		CHECK(set.complete());
	}

	// One hole, no parity at all: that hole and nothing else
	{
		const size_t dropped[] = {5};
		shard_set set = receive(f, dropped, false);
		set.missing_shards(missing, true);
		CHECK(missing.size() == 1);
		CHECK(missing.front() == 5);
	}

	// One hole with its parity in hand: nothing to ask for, the parity already put it
	// back. This is the dedup the whole design turns on — asking for a shard the
	// parity has already rebuilt is bandwidth spent on the path that is losing packets.
	{
		const size_t dropped[] = {5};
		shard_set set = receive(f, dropped, true);
		CHECK(set.complete());
		set.missing_shards(missing, true);
		CHECK(missing.empty());
	}

	// Two holes in one group, parity held: all but one is asked for, because filling
	// those is exactly what brings the last back into the parity's reach
	{
		const size_t dropped[] = {2, 5};
		shard_set set = receive(f, dropped, true);
		CHECK(not set.complete());
		CHECK(set.parity.size() == 1);
		set.missing_shards(missing, true);
		CHECK(missing.size() == 1);
		CHECK(missing.front() == 2);
	}

	// Three holes in one group, parity held: two asked for, one left to the parity
	{
		const size_t dropped[] = {1, 3, 6};
		shard_set set = receive(f, dropped, true);
		set.missing_shards(missing, true);
		CHECK(missing.size() == 2);
		CHECK(missing[0] == 1);
		CHECK(missing[1] == 3);
	}

	// Two holes in one group with the parity *also* lost: both are asked for
	{
		const size_t dropped[] = {2, 5};
		shard_set set = receive(f, dropped, false);
		set.missing_shards(missing, true);
		CHECK(missing.size() == 2);
		CHECK(missing[0] == 2);
		CHECK(missing[1] == 5);
	}

	// Holes in different groups, each group's parity held: each group repairs its own
	// and there is nothing to ask for
	{
		const size_t dropped[] = {1, 9, 17};
		shard_set set = receive(f, dropped, true);
		CHECK(set.complete());
		set.missing_shards(missing, true);
		CHECK(missing.empty());
	}

	// The last shard of the frame is missing and no parity says how long the frame is.
	// Only a frame the caller has decided is over may guess at the index past the end;
	// while it might still be arriving, guessing would ask for a shard that does not
	// exist yet on every single frame.
	{
		const size_t dropped[] = {23};
		shard_set set = receive(f, dropped, false);
		CHECK(not set.complete());
		set.missing_shards(missing, false);
		CHECK(missing.empty());
		set.missing_shards(missing, true);
		CHECK(missing.size() == 1);
		CHECK(missing.front() == 23);
	}

	// A frame nothing has arrived for asks for nothing: there is no evidence it exists
	{
		shard_set empty;
		empty.reset(11);
		empty.missing_shards(missing, true);
		CHECK(missing.empty());
	}

	// The same rules over interleaved groups, where a burst lands one hole in each of
	// several groups: every one of them is the parity's business and none is asked for
	{
		frame strided = make_frame(64, 7, fec::group_size, fec::interleave_depth);
		const size_t dropped[] = {8, 9, 10, 11};
		shard_set set = receive(strided, dropped, true);
		CHECK(set.complete());
		set.missing_shards(missing, true);
		CHECK(missing.empty());

		// A burst one wider than the stride: one group has two holes, and exactly
		// one shard of it is asked for
		const size_t wide[] = {8, 9, 10, 11, 12};
		shard_set set2 = receive(strided, wide, true);
		CHECK(not set2.complete());
		set2.missing_shards(missing, true);
		CHECK(missing.size() == 1);
		CHECK(missing.front() == 8);
	}
}

// The accumulator's rate limit, driven directly: which frames would send a request at
// time `now`, and what it does to their state. Mirrors shard_accumulator::try_nack.
bool would_nack(shard_set & set, int64_t now, int64_t delay, uint8_t max_rounds)
{
	if (set.empty() or set.complete() or set.nack_rounds >= max_rounds)
		return false;
	const XrTime since = std::max(set.last_shard, set.nack_last);
	if (since == 0 or now - since < delay)
		return false;

	std::vector<uint16_t> missing;
	set.missing_shards(missing, true);
	if (missing.empty())
		return false;

	++set.nack_rounds;
	set.nack_last = now;
	return true;
}

void test_rate_limit()
{
	std::printf("Part D: one request per frame per round, two rounds and no more\n");

	const int64_t delay = 2'500'000;
	const uint8_t rounds = 2;

	frame f = make_frame(24, 7, fec::group_size, 1);
	const size_t dropped[] = {2, 5};
	shard_set set = receive(f, dropped, false);
	const XrTime last = set.last_shard;
	CHECK(last != 0);

	// Still arriving: nothing goes out until the frame has been quiet for the delay.
	// The two paths hand a frame over well out of order and a paced frame is spread
	// over milliseconds, so an immediate request would fire on shards still in the air.
	CHECK(not would_nack(set, last + delay / 2, delay, rounds));
	CHECK(set.nack_rounds == 0);

	// Quiet long enough: one round
	CHECK(would_nack(set, last + delay, delay, rounds));
	CHECK(set.nack_rounds == 1);

	// And not a second one until the first has had a round trip to be answered in
	CHECK(not would_nack(set, last + delay + 1, delay, rounds));
	CHECK(not would_nack(set, last + delay + delay - 1, delay, rounds));
	CHECK(would_nack(set, last + 2 * delay, delay, rounds));
	CHECK(set.nack_rounds == 2);

	// Two rounds is the lot. A frame that is not coming back must cost a fixed number
	// of requests and then hand over to the incomplete-frame path that always worked.
	for (int i = 3; i < 20; ++i)
		CHECK(not would_nack(set, last + int64_t(i) * delay, delay, rounds));
	CHECK(set.nack_rounds == 2);

	// A frame that completes stops asking, whatever its round count says
	{
		shard_set whole = receive(f, {}, false);
		CHECK(whole.complete());
		CHECK(not would_nack(whole, whole.last_shard + 10 * delay, delay, rounds));
	}

	// A shard arriving pushes the deadline out: the frame is arriving again, and the
	// hole may be about to fill on its own
	{
		shard_set late = receive(f, dropped, false);
		const XrTime t0 = late.last_shard;
		late.insert(data_shard(f.shards[5]), t0 + delay - 1);
		CHECK(late.last_shard == t0 + delay - 1);
		CHECK(not would_nack(late, t0 + delay, delay, rounds));
	}

	// The slot is reused for the next frame, and the count has to go with it or a
	// stream would stop asking after two frames for the rest of the session
	set.reset(8);
	CHECK(set.nack_rounds == 0);
	CHECK(set.nack_last == 0);
	CHECK(set.last_shard == 0);

	// Two rounds inside one frame's budget: at 90 Hz that is 11 ms, the delay is
	// 2.5 ms, and a LAN round trip is 2-5 ms — so both answers can still land in time
	CHECK(2 * delay + 5'000'000 < 11'000'000);
}

void test_bitmap()
{
	std::printf("Part E: the bitmap on the wire\n");

	// Round trip, scattered
	{
		std::vector<uint16_t> want = {7, 8, 9, 13, 40, 41};
		auto n = make_request(1, 99, want);
		CHECK(n.first_shard_idx == 7);
		CHECK(read_request(n) == want);
		CHECK(n.bitmap.size() <= from_headset::max_nack_bitmap_bytes);
	}

	// A single index
	{
		std::vector<uint16_t> want = {500};
		auto n = make_request(0, 1, want);
		CHECK(n.bitmap.size() == 1);
		CHECK(n.bitmap[0] == 1);
		CHECK(read_request(n) == want);
	}

	// Holes spread wider than the window: truncated at the cap rather than dropped or
	// grown. A frame missing more than 256 shards from its first hole is not one a
	// retransmission round was going to save.
	{
		std::vector<uint16_t> want;
		for (uint16_t i = 0; i < 600; i += 3)
			want.push_back(i);
		auto n = make_request(0, 1, want);
		CHECK(n.bitmap.size() <= from_headset::max_nack_bitmap_bytes);
		auto back = read_request(n);
		CHECK(not back.empty());
		CHECK(back.size() < want.size());
		// Everything it does name is one of the holes, and the lowest ones first
		for (size_t i = 0; i < back.size(); ++i)
			CHECK(back[i] == want[i]);
	}

	// The whole request is small enough to be worth putting on the datagram path,
	// which is the reason it goes there rather than on the control socket
	{
		std::vector<uint16_t> want;
		for (uint16_t i = 0; i < 256; ++i)
			want.push_back(i);
		auto n = make_request(0, 1, want);
		CHECK(n.bitmap.size() == from_headset::max_nack_bitmap_bytes);
		CHECK(serialized_size(n) < 64);
		std::printf("  a full 256-shard request serializes to %zu B\n", serialized_size(n));
	}

	// And it survives the real wire encoding
	{
		std::vector<uint16_t> want = {3, 4, 90};
		auto n = make_request(2, 12345, want);

		serialization_packet packet;
		packet.serialize(n);
		std::vector<uint8_t> flat;
		for (const auto & span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
			flat.insert(flat.end(), span.begin(), span.end());
		auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[flat.size() + 1]);
		std::memcpy(memory.get(), flat.data(), flat.size());
		deserialization_packet in{memory, std::span<uint8_t>(memory.get(), flat.size())};
		auto back = in.deserialize<from_headset::nack>();

		CHECK(back.stream_index == 2);
		CHECK(back.frame_idx == 12345);
		CHECK(back.first_shard_idx == 3);
		CHECK(read_request(back) == want);
	}
}

// The UI permits a 1 Gbit/s aggregate stream. At 90 Hz and two equal eyes,
// a repair two source frames back must not be lost to the old 1 MiB byte ring.
// Synthetic payload budget only; this does not assert actual Wi-Fi delivery.
void test_high_bitrate_history()
{
	shard_history h;
	h.set_enabled(true);
	constexpr size_t bytes = 1'000'000'000ULL / 8 / 90 / 2;
	std::vector<uint8_t> payload(bytes), blob;
	uint16_t shards = 0;
	for (uint64_t frame = 10; frame <= 12; ++frame)
	{
		std::fill(payload.begin(), payload.end(), uint8_t(frame));
		size_t offset = 0;
		uint16_t index = 0;
		while (offset < bytes)
		{
			data_shard shard{};
			shard.frame_idx = frame;
			shard.shard_idx = index;
			if (index == 0)
				shard.view_info.emplace();
			size_t size = std::min(bytes - offset, fec::shard_payload_budget(true) - serialized_size(shard.view_info));
			shard.payload = std::span(payload).subspan(offset, size);
			if (offset + size == bytes)
				shard.timing_info.emplace();
			fec::encode_blob(shard, blob);
			h.push(frame, index, blob, true);
			offset += size;
			++index;
		}
		shards = index;
	}
	size_t retained = 0, retained_bytes = 0;
	for (uint16_t first = 0; first < shards; first += 64)
	{
		std::array<uint8_t, 8> bitmap;
		bitmap.fill(0xff);
		std::vector<shard_history::hit> hits;
		h.collect(10, first, bitmap, 64, hits);
		for (const auto & hit: hits)
		{
			auto shard = fec::decode_blob(0, 10, hit.shard_idx, hit.blob);
			CHECK(std::all_of(shard.payload.begin(), shard.payload.end(), [](uint8_t v) { return v == 10; }));
			++retained;
			retained_bytes += shard.payload.size();
		}
	}
	CHECK(retained == shards);
	CHECK(retained_bytes == bytes);
	CHECK(h.held() <= shard_history::max_entries);
}

} // namespace

int main()
{
	test_history_ring();
	test_high_bitrate_history();
	test_serving_a_request();
	test_gap_detection();
	test_rate_limit();
	test_bitmap();

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
