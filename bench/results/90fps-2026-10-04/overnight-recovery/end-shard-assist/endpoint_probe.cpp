// Cached end-shard replay. Run with the accompanying run.sh.
// Fixture helpers adapted from WiVRn NX tests/nack_test.cpp.
// Uses the production history selector; no socket or private encoder method.

#include "fec.h"
#include "nack_deadline.h"
#include "shard_history.h"
#include "shard_set.h"
#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
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

size_t bit_count(std::span<const uint8_t> bitmap)
{
    size_t count = 0;
    for (uint8_t byte: bitmap)
        count += std::popcount(byte);
    return count;
}

// Scratch-only server adapter: mirrors the existing requested-shard collect, then
// optionally adds the source-counted final shard iff its real blob has timing_info.
std::vector<data_shard> serve_with_end_assist(shard_history & history,
                                              const from_headset::nack & request,
                                              size_t budget,
                                              bool assist)
{
    history.note_nacked(request.frame_idx, uint32_t(bit_count(request.bitmap)));
    std::vector<shard_history::hit> hits;
    history.collect(request.frame_idx, request.first_shard_idx, request.bitmap, budget, hits);
    std::vector<data_shard> out;
    for (const auto & hit: hits)
        out.push_back(fec::decode_blob(request.stream_index, request.frame_idx, hit.shard_idx, hit.blob));

    if (not assist or hits.empty() or hits.size() >= budget)
        return out;
    auto tail = history.collect_frame_end_candidate(request.frame_idx, hits, budget - hits.size());
    if (not tail)
        return out;
    data_shard recovered = fec::decode_blob(request.stream_index, request.frame_idx, tail->shard_idx, tail->blob);
    if (not recovered.timing_info)
        return out;
    out.push_back(std::move(recovered));
    return out;
}

from_headset::nack wire_roundtrip(const from_headset::nack & request)
{
    serialization_packet packet;
    packet.serialize(request);
    std::vector<uint8_t> flat;
    for (const auto & span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
        flat.insert(flat.end(), span.begin(), span.end());
    auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[flat.size() + 1]);
    std::memcpy(memory.get(), flat.data(), flat.size());
    deserialization_packet input{memory, std::span<uint8_t>(memory.get(), flat.size())};
    auto decoded = input.deserialize<from_headset::nack>();
    CHECK(decoded.stream_index == request.stream_index && decoded.frame_idx == request.frame_idx);
    CHECK(decoded.first_shard_idx == request.first_shard_idx && read_request(decoded) == read_request(request));
    return decoded;
}

void send_frame_transport(shard_history & h, const frame & f, bool final_on_primary = true, bool finish = true)
{
    std::vector<uint8_t> blob;
    for (size_t i = 0; i < f.shards.size(); ++i)
    {
        const auto & shard = f.shards[i];
        fec::encode_blob(shard, blob);
        h.push(shard.frame_idx, shard.shard_idx, blob, i + 1 != f.shards.size() or final_on_primary);
    }
    if (finish)
        h.end_frame(f.shards.front().frame_idx, uint32_t(f.shards.size()));
}

size_t present_count(const shard_set & set)
{
    return std::ranges::count_if(set.shards(), [](const auto & shard) { return bool(shard); });
}

struct replay_result
{
    size_t recovered = 0;
    size_t replies = 0;
    size_t bytes = 0;
    size_t rounds = 0;
    size_t nacked = 0;
    size_t duplicates = 0;
    bool complete = false;
};

replay_result replay_tail(size_t tail_loss, bool assist)
{
    constexpr uint64_t frame_id = 42;
    constexpr size_t shard_count = 256;
    frame f = make_frame(shard_count, frame_id, fec::clean_group_size, fec::interleave_depth);
    shard_history history;
    history.set_enabled(true);
    send_frame_transport(history, f);
    shard_set received(0);
    int64_t now = 1'000'000;
    for (size_t i = 0; i < shard_count - tail_loss; ++i)
        received.insert(data_shard(f.shards[i]), now++);

    replay_result result;
    for (uint8_t round = 0; round < nack_max_rounds; ++round)
    {
        std::vector<uint16_t> missing;
        received.missing_shards(missing, true); // newer frame proves this frame's end
        if (missing.empty()) break;
        auto request = make_request(0, frame_id, missing);
        request = wire_roundtrip(request);
        auto deadline = nack_poll_deadline(now, received.last_shard, received.nack_last,
                                           received.nack_rounds, true, true, !received.empty(),
                                           received.complete(), [&] {
                                               std::vector<uint16_t> current;
                                               received.missing_shards(current, true);
                                               return !current.empty();
                                           });
        if (not deadline) break;
        now = *deadline;
        received.nack_last = now;
        ++received.nack_rounds;
        ++result.rounds;
        auto replies = serve_with_end_assist(history, request, 64, assist);
        CHECK(replies.size() <= 64);
        CHECK(!replies.empty() && replies.front().shard_idx == missing.front());
        for (auto & reply: replies)
        {
            result.bytes += serialized_size(reply);
            ++result.replies;
            const uint16_t index = reply.shard_idx;
            CHECK(same_shard(reply, f.shards[index]));
            if (index == shard_count - 1)
                CHECK(reply.timing_info && reply.timing_info->encode_begin == f.shards[index].timing_info->encode_begin &&
                      reply.timing_info->encode_end == f.shards[index].timing_info->encode_end &&
                      reply.timing_info->send_begin == f.shards[index].timing_info->send_begin &&
                      reply.timing_info->send_end == f.shards[index].timing_info->send_end);
            const bool duplicate = index < received.shards().size() && bool(received.shards()[index]);
            const auto inserted = received.insert(std::move(reply), ++now);
            if (duplicate) { ++result.duplicates; CHECK(!inserted); }
        }
        result.complete = received.complete();
        if (result.complete) break;
    }
    result.recovered = present_count(received) - (shard_count - tail_loss);
    result.complete = received.complete();
    if (auto cost = history.frame_cost(frame_id)) result.nacked = cost->shards_nacked;
    return result;
}

void end_assist_gate()
{
    std::puts("mode,tail_loss,rounds,replies,duplicate_replies,recovered_tail,ready,nacked_metric,serialized_reply_bytes");
    for (size_t tail: {1, 2, 16, 64, 128})
        for (bool assist: {false, true})
        {
            const auto r = replay_tail(tail, assist);
            std::printf("%s,%zu,%zu,%zu,%zu,%zu,%u,%zu,%zu\n", assist ? "assist" : "baseline", tail,
                        r.rounds, r.replies, r.duplicates, r.recovered, unsigned(r.complete), r.nacked, r.bytes);
        }

    auto new_history = [] { auto h = std::make_unique<shard_history>(); h->set_enabled(true); return h; };

    // A real interior NACK can carry the actual end shard as assist without the
    // client naming or guessing its index. No newer-frame/tail inference is used.
    {
        constexpr uint64_t id = 81;
        frame f = make_frame(256, id, fec::clean_group_size, fec::interleave_depth);
        for (bool assist: {false, true})
        {
            auto h = new_history(); send_frame_transport(*h, f);
            shard_set rx(0);
            for (uint16_t i = 0; i < 255; ++i)
                if (i != 7) rx.insert(data_shard(f.shards[i]), 10'000'000 + i);
            std::vector<uint16_t> holes;
            rx.missing_shards(holes, false);
            CHECK(holes == std::vector<uint16_t>{7});
            auto request = wire_roundtrip(make_request(0, id, holes));
            CHECK(read_request(request) == holes);
            auto replies = serve_with_end_assist(*h, request, 64, assist);
            CHECK(replies.size() == (assist ? 2 : 1));
            for (auto & reply: replies) rx.insert(std::move(reply), 11'000'000);
            CHECK(rx.complete() == assist);
        }
    }

    // A newest contiguous prefix with only its final metadata shard absent creates
    // no ordinary NACK: the server-only assist needs some valid existing request.
    {
        frame f = make_frame(256, 82, fec::clean_group_size, fec::interleave_depth);
        shard_set rx(0);
        for (uint16_t i = 0; i < 255; ++i) rx.insert(data_shard(f.shards[i]), 12'000'000 + i);
        std::vector<uint16_t> holes;
        rx.missing_shards(holes, false);
        CHECK(holes.empty() && !rx.complete());
        CHECK(!nack_poll_deadline(rx.last_shard + nack_quiet_period_ns, rx.last_shard,
                                  rx.nack_last, rx.nack_rounds, true, true, true, false,
                                  [] { return false; }));
        CHECK(nack_poll_timeout(rx.last_shard + nack_quiet_period_ns, std::nullopt,
                                std::chrono::milliseconds(100)).count() == 100);
    }

    constexpr uint64_t frame_id = 80;
    frame f = make_frame(256, frame_id, fec::clean_group_size, fec::interleave_depth);
    {
        auto h = new_history(); send_frame_transport(*h, f);
        // Requested final marker is returned once, never duplicated by assistance.
        auto request = make_request(0, frame_id, std::vector<uint16_t>{255});
        auto out = serve_with_end_assist(*h, request, 64, true);
        CHECK(out.size() == 1 && out.front().timing_info);
        request = make_request(0, frame_id, std::vector<uint16_t>{7, 255});
        out = serve_with_end_assist(*h, request, 64, true);
        CHECK(out.size() == 2 && out[0].shard_idx == 7 && out[1].shard_idx == 255);
    }
    {
        auto h = new_history(); send_frame_transport(*h, f);
        auto request = make_request(0, frame_id, std::vector<uint16_t>{7});
        auto out = serve_with_end_assist(*h, request, 1, true);
        CHECK(out.size() == 1 && out.front().shard_idx == 7); // no room over caller budget
    }
    {
        auto h = new_history(); send_frame_transport(*h, f);
        auto request = make_request(0, frame_id, std::vector<uint16_t>{500});
        CHECK(serve_with_end_assist(*h, request, 64, true).empty()); // no requested hit => no assist
        CHECK(serve_with_end_assist(*h, make_request(0, frame_id, std::vector<uint16_t>{7}), 0, true).empty());
        CHECK(serve_with_end_assist(*h, make_request(0, frame_id + 1, std::vector<uint16_t>{7}), 64, true).empty());
    }
    {
        auto h = new_history(); send_frame_transport(*h, f, false);
        auto out = serve_with_end_assist(*h, make_request(0, frame_id, std::vector<uint16_t>{7}), 64, true);
        CHECK(out.size() == 1 && out.front().shard_idx == 7); // final on TCP is not in UDP history
    }
    {
        auto h = new_history(); send_frame_transport(*h, f, true, false);
        auto out = serve_with_end_assist(*h, make_request(0, frame_id, std::vector<uint16_t>{7}), 64, true);
        CHECK(out.size() == 1 && out.front().shard_idx == 7); // no end_frame count => no candidate
    }
    {
        auto h = new_history(); send_frame_transport(*h, f);
        h->end_frame(frame_id, uint32_t(std::numeric_limits<uint16_t>::max()) + 2);
        auto out = serve_with_end_assist(*h, make_request(0, frame_id, std::vector<uint16_t>{7}), 64, true);
        CHECK(out.size() == 1 && out.front().shard_idx == 7); // reject count before narrowing
    }
    {
        auto h = new_history();
        frame no_marker = f; no_marker.shards.back().timing_info.reset();
        send_frame_transport(*h, no_marker);
        auto out = serve_with_end_assist(*h, make_request(0, frame_id, std::vector<uint16_t>{7}), 64, true);
        CHECK(out.size() == 1 && out.front().shard_idx == 7); // counted end index is not trusted without marker
    }
    {
        auto h = new_history(); send_frame_transport(*h, f);
        std::vector<uint8_t> filler(1400, 0x5a), blob;
        for (uint16_t i = 0; i < 1600; ++i) h->push(43, i, filler, true); // evict frame 80 payload, retain its count slot
        auto out = serve_with_end_assist(*h, make_request(0, frame_id, std::vector<uint16_t>{7}), 64, true);
        CHECK(out.empty()); // original hit is evicted too, so no assistant
    }
    {
        auto h = new_history(); send_frame_transport(*h, f);
        auto set = receive(f, std::array<size_t, 1>{255}, true); // actual FEC reconstructs terminal metadata
        CHECK(set.complete());
        std::vector<uint16_t> missing;
        set.missing_shards(missing, false);
        CHECK(missing.empty()); // no request means no server assistance needed
    }

    CHECK(replay_tail(2, false).rounds == 2 && replay_tail(2, false).complete);
    CHECK(replay_tail(2, true).rounds == 1 && replay_tail(2, true).complete);
    CHECK(!replay_tail(128, false).complete && !replay_tail(128, true).complete);
    CHECK(replay_tail(128, true).rounds == nack_max_rounds);
    shard_set exhausted(0);
    data_shard e0{}; e0.frame_idx = 99; e0.shard_idx = 0;
    data_shard e2 = e0; e2.shard_idx = 2;
    exhausted.insert(std::move(e0), 20'000'000);
    exhausted.insert(std::move(e2), 20'000'000);
    exhausted.nack_rounds = nack_max_rounds;
    CHECK(!nack_poll_deadline(exhausted.last_shard + nack_quiet_period_ns,
                              exhausted.last_shard, exhausted.nack_last,
                              exhausted.nack_rounds, true, true, true, false,
                              [] { return true; }));
}


} // namespace

int main()
{
    end_assist_gate();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
