// Forward error correction for the video stream: the parity scheme, checked
// without a headset, a GPU or a socket.
//
// Part A: group construction. A frame is sharded the way video_encoder::SendData
// shards it (view_info on the first shard, timing_info on the last, the FEC payload
// budget) and fed to fec::group_builder. The groups it produces must cover every
// shard exactly once, in order, with no gaps and no overlap, and a parity shard
// must never come out as a bigger datagram than a data shard would have been
// without FEC — the whole point of fec::payload_reserve.
// Part B: parity round trip. Every shard position of every group is dropped in
// turn, the parity shard is put through the real serialization, and what comes back
// must be byte for byte the shard that was dropped — payload, view_info and
// timing_info alike.
// Part C: graceful failure. Two erasures in one group, a parity shard on its own,
// an empty group, and a parity shard whose length table has been corrupted must all
// produce no reconstruction and no crash.
// Part D: dedup. A rebuilt shard goes in through the same door as a received one,
// so the real copy arriving late is an ordinary duplicate; either arrival order has
// to leave the frame identical.
// Part E: variable group sizes. The adaptive ratio hands group_builder a K of 4, 8
// or 16; every one of them has to cover the frame exactly once, keep the overhead at
// 1/(K+1), keep a parity datagram no bigger than a data one — which is what makes
// fec::payload_reserve scale with K — and round trip every position of every group.
// Part F: interleaving. With the groups strided, a burst of consecutive datagrams
// lost on the air lands one erasure in each of several groups, all of them
// repairable; the same burst against contiguous groups is not. Both halves are
// checked, because the second is what the first is for.
// Part G: the ratio controller. Clean links buy the cheap ratio, loss buys the
// protective one immediately, and the way back is slow enough that a link sitting on
// a threshold cannot flap it.
//
// Build (the boost include is wherever the build fetched it):
//   g++ -std=c++23 -I common -I build-client/common -I external \
//       -I build-client/_deps/boost-src/libs/pfr/include \
//       -o fec_test tests/fec_test.cpp common/smp.cpp -lcrypto
//   ./fec_test

#include "fec.h"
#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <thread>
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
using view_info_t = data_shard::view_info_t;
using timing_info_t = data_shard::timing_info_t;

struct bool_record
{
	uint32_t prefix;
	bool enabled;
	uint16_t suffix;
};

template <typename T>
T round_trip(const T & value)
{
	serialization_packet packet;
	packet.serialize(value);

	std::vector<uint8_t> flat;
	for (const auto & span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
		flat.insert(flat.end(), span.begin(), span.end());

	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[flat.size() + 1]);
	std::memcpy(memory.get(), flat.data(), flat.size());

	deserialization_packet in{memory, std::span<uint8_t>(memory.get(), flat.size())};
	return in.deserialize<T>();
}

view_info_t make_view_info()
{
	return view_info_t{
	        .display_time = 123'456'789,
	        .pose = {XrPosef{{0, 0, 0, 1}, {0.03f, 0, 0}}, XrPosef{{0, 0, 0, 1}, {-0.03f, 0, 0}}},
	        .fov = {XrFovf{-0.9f, 0.9f, 0.9f, -0.9f}, XrFovf{-0.9f, 0.9f, 0.9f, -0.9f}},
	        .foveation = {to_headset::foveation_parameter{{1, 4, 5, 3, 1}, {2, 3, 4, 3, 2}},
	                      to_headset::foveation_parameter{{1, 4, 5, 3, 1}, {2, 3, 4, 3, 2}}},
	        .alpha = false,
	        .quad = {},
	};
}

// One encoded frame, sharded exactly the way video_encoder::SendData shards it.
struct frame
{
	std::vector<uint8_t> encoded;
	std::vector<data_shard> shards;
	std::vector<parity_shard> parity;
	// Buffers the parity payloads point into, kept alive alongside them
	std::vector<std::vector<uint8_t>> parity_payloads;
};

frame make_frame(size_t bytes,
                 uint8_t stream_idx = 0,
                 uint64_t frame_idx = 42,
                 uint16_t k = fec::group_size,
                 uint16_t depth = 1)
{
	frame f;
	f.encoded.resize(bytes);
	for (size_t i = 0; i < bytes; ++i)
		f.encoded[i] = uint8_t(i * 7 + (i >> 8) * 31 + 3);

	fec::group_builder builder;
	builder.set_layout(k, depth);
	builder.reset(stream_idx, frame_idx);

	data_shard shard;
	shard.stream_item_idx = stream_idx;
	shard.frame_idx = frame_idx;
	shard.shard_idx = 0;
	shard.view_info = make_view_info();

	// A block owes one parity per group of it, and it is the last, empty-handed take()
	// that opens the next block: the sender drains, it does not take once.
	auto take_parity = [&] {
		while (auto p = builder.take())
		{
			// The builder's buffer is reused, so keep our own copy of the payload
			f.parity_payloads.emplace_back(p->payload.begin(), p->payload.end());
			p->payload = f.parity_payloads.back();
			f.parity.push_back(std::move(*p));
		}
	};

	size_t offset = 0;
	while (offset < bytes)
	{
		const size_t budget = fec::shard_payload_budget(true, k) - serialized_size(shard.view_info);
		const size_t next = std::min(bytes, offset + budget);
		if (next == bytes)
			shard.timing_info = timing_info_t{1, 2, 3, 4};
		shard.payload = std::span<uint8_t>(f.encoded).subspan(offset, next - offset);

		f.shards.push_back(shard);
		builder.add(shard);
		if (builder.block_full())
			take_parity();

		++shard.shard_idx;
		shard.view_info.reset();
		offset = next;
	}
	take_parity();

	// The shards' payloads point into f.encoded, which moves with the frame; repoint
	// them so the returned value is self contained.
	auto moved = std::move(f);
	for (size_t i = 0, off = 0; i < moved.shards.size(); ++i)
	{
		const size_t n = moved.shards[i].payload.size();
		moved.shards[i].payload = std::span<uint8_t>(moved.encoded).subspan(off, n);
		off += n;
	}
	for (size_t i = 0; i < moved.parity.size(); ++i)
		moved.parity[i].payload = moved.parity_payloads[i];
	return moved;
}

// What reconstruct() is given: every shard of the frame except the dropped ones
auto lookup_without(const frame & f, std::span<const size_t> dropped)
{
	return [&f, dropped](uint16_t idx) -> const data_shard * {
		for (size_t d: dropped)
			if (d == idx)
				return nullptr;
		if (idx >= f.shards.size())
			return nullptr;
		return &f.shards[idx];
	};
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
	if (a.view_info and a.view_info->foveation[0].x != b.view_info->foveation[0].x)
		return false;
	if (a.timing_info and (a.timing_info->encode_begin != b.timing_info->encode_begin or
	                       a.timing_info->send_end != b.timing_info->send_end))
		return false;
	return true;
}

void test_group_construction()
{
	std::printf("Part A: group construction\n");

	// About 200 kB, a frame at 150 Mbit/s and 90 fps
	frame f = make_frame(200 * 1024);

	CHECK(f.shards.size() > 150);
	CHECK(f.parity.size() == (f.shards.size() + fec::group_size - 1) / fec::group_size);

	// Every shard is covered exactly once, in order, with no gap
	size_t expected_first = 0;
	size_t covered = 0;
	for (const parity_shard & p: f.parity)
	{
		CHECK(p.stream_item_idx == f.shards.front().stream_item_idx);
		CHECK(p.frame_idx == f.shards.front().frame_idx);
		CHECK(p.first_shard_idx == expected_first);
		CHECK(not p.blob_size.empty());
		CHECK(p.blob_size.size() <= fec::group_size);

		// Only the last group may be short
		if (&p != &f.parity.back())
			CHECK(p.blob_size.size() == fec::group_size);

		// The parity payload is as long as the longest blob of its group, no more
		size_t longest = 0;
		for (uint16_t s: p.blob_size)
			longest = std::max<size_t>(longest, s);
		CHECK(p.payload.size() == longest);

		expected_first += p.blob_size.size();
		covered += p.blob_size.size();
	}
	CHECK(covered == f.shards.size());

	// The overhead is one shard in group_size, i.e. 12.5% for a group of 8
	size_t data_bytes = 0;
	for (const data_shard & s: f.shards)
		data_bytes += serialized_size(s);
	size_t parity_bytes = 0;
	for (const parity_shard & p: f.parity)
		parity_bytes += serialized_size(p);
	const double overhead = double(parity_bytes) / data_bytes;
	CHECK(overhead > 0.11 and overhead < 0.14);
	std::printf("  %zu shards, %zu parity, %.1f%% overhead\n",
	            f.shards.size(),
	            f.parity.size(),
	            overhead * 100);

	// A parity datagram is never larger than the data datagram the same encoder
	// would have produced with FEC off — that is what payload_reserve buys, and it
	// is the property that keeps FEC from causing the fragmentation it exists to
	// survive.
	size_t largest_parity = 0;
	for (const parity_shard & p: f.parity)
		largest_parity = std::max(largest_parity, serialized_size(p));
	size_t largest_data = 0;
	for (const data_shard & s: f.shards)
		largest_data = std::max(largest_data, serialized_size(s));

	// The same shard with FEC off would carry payload_reserve bytes more
	CHECK(largest_parity <= largest_data + fec::payload_reserve(fec::group_size));
	std::printf("  largest parity %zu B, largest data %zu B, reserve %zu B\n",
	            largest_parity,
	            largest_data,
	            fec::payload_reserve(fec::group_size));

	// The first shard is the one that carries the pose and the last one the
	// timings, which is what makes them the two the frame cannot do without
	CHECK(f.shards.front().view_info.has_value());
	CHECK(not f.shards.front().timing_info.has_value());
	CHECK(f.shards.back().timing_info.has_value());

	// A frame small enough to fit in a single shard still gets its parity: it is a
	// handful of bytes and losing it costs a whole frame plus an IDR round trip.
	frame tiny = make_frame(300);
	CHECK(tiny.shards.size() == 1);
	CHECK(tiny.parity.size() == 1);
	CHECK(tiny.parity.front().blob_size.size() == 1);

	// A group that straddles the frame's end is short, not padded out
	frame odd = make_frame(fec::shard_payload_budget(true) * 9 + 100);
	CHECK(odd.parity.size() == 2);
	CHECK(odd.parity.front().blob_size.size() == fec::group_size);
	CHECK(odd.parity.back().blob_size.size() == odd.shards.size() - fec::group_size);
}

void test_round_trip()
{
	std::printf("Part B: parity round trip\n");

	frame f = make_frame(64 * 1024);
	CHECK(f.parity.size() >= 4);

	size_t rebuilt = 0;
	for (const parity_shard & original: f.parity)
	{
		// Through the real wire encoding, spans and all
		parity_shard p = round_trip(original);
		CHECK(p.first_shard_idx == original.first_shard_idx);
		CHECK(p.blob_size == original.blob_size);
		CHECK(p.payload.size() == original.payload.size());

		for (size_t i = 0; i < p.blob_size.size(); ++i)
		{
			const size_t dropped = p.first_shard_idx + i;
			auto shard = fec::reconstruct(p, lookup_without(f, std::span(&dropped, 1)));

			CHECK(shard.has_value());
			if (not shard)
				continue;
			++rebuilt;
			CHECK(same_shard(*shard, f.shards[dropped]));
		}
	}
	std::printf("  rebuilt %zu shards, one per position of every group\n", rebuilt);
	CHECK(rebuilt == f.shards.size());

	// The two special shards specifically: the pose and the timings survive being
	// rebuilt, without which a recovered frame would never be submitted
	{
		const size_t dropped = 0;
		auto first = fec::reconstruct(f.parity.front(), lookup_without(f, std::span(&dropped, 1)));
		CHECK(first.has_value());
		CHECK(first and first->view_info.has_value());
		CHECK(first and first->view_info->display_time == make_view_info().display_time);
		CHECK(first and first->view_info->foveation[1].y == make_view_info().foveation[1].y);
	}
	{
		const size_t dropped = f.shards.size() - 1;
		auto last = fec::reconstruct(f.parity.back(), lookup_without(f, std::span(&dropped, 1)));
		CHECK(last.has_value());
		CHECK(last and last->timing_info.has_value());
		CHECK(last and last->timing_info->send_end == 4);
		// The parity shard is also what tells the headset that this index exists at
		// all: without it a lost last shard is not even known to be missing.
		CHECK(f.parity.back().first_shard_idx + f.parity.back().blob_size.size() == f.shards.size());
	}

	// A reconstructed shard owns its bytes: it stays valid once whatever it was
	// rebuilt from is gone
	std::optional<data_shard> detached;
	{
		frame temp = make_frame(8 * 1024);
		const size_t dropped = 3;
		detached = fec::reconstruct(temp.parity.front(), lookup_without(temp, std::span(&dropped, 1)));
		CHECK(detached.has_value());
	}
	CHECK(detached and detached->payload.size() > 0);
	CHECK(detached and detached->shard_idx == 3);
}

void test_recovery_blob_survives_send_mutation()
{
	std::printf("Part B2: recovery snapshot survives in-place send encryption\n");
	constexpr size_t shard_count = fec::group_size;
	std::vector<std::vector<uint8_t>> sent_payloads(shard_count), original_payloads(shard_count);
	std::vector<std::vector<uint8_t>> recovery_blobs(shard_count);
	std::vector<data_shard> original_shards(shard_count);
	fec::group_builder builder;
	builder.reset(0, 99);
	for (size_t i = 0; i < shard_count; ++i)
	{
		sent_payloads[i].resize(128 + i);
		for (size_t j = 0; j < sent_payloads[i].size(); ++j)
			sent_payloads[i][j] = uint8_t(i * 29 + j * 7);
		original_payloads[i] = sent_payloads[i];
		data_shard shard;
		shard.stream_item_idx = 0;
		shard.frame_idx = 99;
		shard.shard_idx = uint16_t(i);
		if (i == 0)
			shard.view_info = make_view_info();
		if (i + 1 == shard_count)
			shard.timing_info = timing_info_t{1, 2, 3, 4};
		shard.payload = sent_payloads[i];
		original_shards[i] = shard;
		original_shards[i].payload = original_payloads[i];
		fec::encode_blob(shard, recovery_blobs[i]); // before UDP encryption mutates the borrowed span
		for (uint8_t & b: sent_payloads[i])
			b ^= 0xa5; // the socket send happens before the recovery blob is consumed
		builder.add_blob(shard, recovery_blobs[i], true);
	}
	auto parity = builder.take();
	CHECK(parity.has_value());
	if (not parity)
		return;
	std::vector<uint8_t> parity_payload(parity->payload.begin(), parity->payload.end());
	parity->payload = parity_payload;

	const size_t dropped = 3;
	auto rebuilt = fec::reconstruct(*parity, [&](uint16_t idx) -> const data_shard * {
		return idx == dropped ? nullptr : &original_shards[idx];
	});
	CHECK(rebuilt.has_value());
	CHECK(rebuilt and same_shard(*rebuilt, original_shards[dropped]));
	const auto saved_last = fec::decode_blob(0, 99, uint16_t(shard_count - 1), recovery_blobs.back());
	CHECK(same_shard(saved_last, original_shards.back()));
	std::vector<uint8_t> encrypted_blob;
	data_shard mutated = original_shards[dropped];
	mutated.payload = sent_payloads[dropped];
	fec::encode_blob(mutated, encrypted_blob);
	CHECK(encrypted_blob != recovery_blobs[dropped]); // snapshot is preserved across the send mutation
}

void test_span_xor_matches_encoded_blob()
{
	std::printf("Part B3: direct span XOR matches the recovery wire bytes\n");
	for (size_t payload_size: {size_t(0), size_t(7), size_t(32), size_t(1400)})
	{
		data_shard shard;
		shard.view_info = make_view_info();
		shard.timing_info = timing_info_t{.send_begin = 11, .send_end = 22};
		std::vector<uint8_t> payload(payload_size);
		for (size_t i = 0; i < payload_size; ++i)
			payload[i] = uint8_t(i * 37 + 9);
		shard.payload = payload;
		std::vector<uint8_t> expected;
		fec::encode_blob(shard, expected);
		std::vector<uint8_t> recovered(expected.size(), 0xa5);
		CHECK(fec::xor_blob_into(shard, recovered, expected.size()));
		for (size_t i = 0; i < expected.size(); ++i)
			CHECK(uint8_t(recovered[i] ^ 0xa5) == expected[i]);
		std::vector<uint8_t> unchanged(expected.size(), 0x5a);
		CHECK(not fec::xor_blob_into(shard, unchanged, expected.size() + 1));
		CHECK(std::ranges::all_of(unchanged, [](uint8_t b) { return b == 0x5a; }));
	}
}

void test_reused_blob_metadata_threads()
{
	std::printf("Part B5: reused metadata changes shape independently on four threads\n");
	std::array<bool, 4> good{};
	std::vector<std::thread> threads;
	for (size_t worker = 0; worker < good.size(); ++worker)
		threads.emplace_back([&, worker] {
			good[worker] = true;
			for (size_t iteration = 0; iteration < 200; ++iteration)
			{
				data_shard shard{};
				if (iteration % 2)
					shard.view_info = make_view_info();
				if (iteration % 3)
					shard.timing_info = timing_info_t{.send_begin = int64_t(worker), .send_end = int64_t(iteration)};
				const size_t sizes[] = {0, 7, 31, 32, 1400};
				std::vector<uint8_t> payload(sizes[iteration % 5], uint8_t(iteration + worker));
				shard.payload = payload;
				serialization_packet independent;
				independent.serialize(shard.view_info);
				independent.serialize(shard.timing_info);
				independent.serialize(shard.payload);
				std::vector<uint8_t> expected;
				for (const auto span: static_cast<std::vector<std::span<uint8_t>> &>(independent))
					expected.insert(expected.end(), span.begin(), span.end());
				std::vector<uint8_t> encoded, recovered(expected.size(), 0);
				fec::encode_blob(shard, encoded);
				if (encoded != expected or not fec::xor_blob_into(shard, recovered, expected.size()) or recovered != expected)
					good[worker] = false;
			}
		});
	for (auto & thread: threads)
		thread.join();
	for (const bool result: good)
		CHECK(result);
}

void test_malformed_bool_deserialization()
{
	std::printf("Part B4: malformed bool bytes are rejected before materialization\n");
	for (uint8_t value: {uint8_t(0), uint8_t(1)})
	{
		auto memory = std::make_shared<uint8_t[]>(1);
		memory[0] = value;
		deserialization_packet packet{memory, std::span<uint8_t>(memory.get(), 1)};
		CHECK(packet.deserialize<bool>() == (value != 0));
	}
	for (uint8_t value: {uint8_t(2), uint8_t(254), uint8_t(255)})
	{
		auto memory = std::make_shared<uint8_t[]>(1);
		memory[0] = value;
		deserialization_packet packet{memory, std::span<uint8_t>(memory.get(), 1)};
		bool rejected = false;
		try
		{
			(void)packet.deserialize<bool>();
		}
		catch (const deserialization_error &)
		{
			rejected = true;
		}
		CHECK(rejected);
	}

	// A bool between ordinary scalar fields must take the checked, ungrouped path.
	bool_record original{.prefix = 0x12345678, .enabled = true, .suffix = 0x4321};
	serialization_packet encoded;
	encoded.serialize(original);
	std::vector<uint8_t> bytes;
	for (const auto span: static_cast<std::vector<std::span<uint8_t>> &>(encoded))
		bytes.insert(bytes.end(), span.begin(), span.end());
	CHECK(bytes.size() == sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint16_t));
	if (bytes.size() == sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint16_t))
	{
		auto memory = std::make_shared<uint8_t[]>(bytes.size() + 1);
		std::memcpy(memory.get(), bytes.data(), bytes.size());
		memory[sizeof(uint32_t)] = 254;
		deserialization_packet packet{memory, std::span<uint8_t>(memory.get(), bytes.size())};
		bool rejected = false;
		try
		{
			(void)packet.deserialize<bool_record>();
		}
		catch (const deserialization_error &)
		{
			rejected = true;
		}
		CHECK(rejected);
	}
}

void test_graceful_failure()
{
	std::printf("Part C: what a single parity cannot do\n");

	frame f = make_frame(64 * 1024);
	const parity_shard & p = f.parity.front();

	// Two erasures in one group: XOR recovers one, and refusing is the only correct
	// answer. Notably it must not hand back a plausible looking shard of garbage.
	for (size_t i = 0; i + 1 < p.blob_size.size(); ++i)
	{
		const size_t dropped[2] = {p.first_shard_idx + i, p.first_shard_idx + i + 1};
		CHECK(not fec::reconstruct(p, lookup_without(f, dropped)).has_value());
	}

	// Two erasures far apart in the same group
	{
		const size_t dropped[2] = {size_t(p.first_shard_idx), size_t(p.first_shard_idx) + p.blob_size.size() - 1};
		CHECK(not fec::reconstruct(p, lookup_without(f, dropped)).has_value());
	}

	// A parity shard on its own, with none of its group: nothing to do
	{
		std::vector<size_t> all;
		for (size_t i = 0; i < p.blob_size.size(); ++i)
			all.push_back(p.first_shard_idx + i);
		CHECK(not fec::reconstruct(p, lookup_without(f, all)).has_value());
	}

	// A whole group, nothing missing: nothing to do either
	CHECK(not fec::reconstruct(p, lookup_without(f, {})).has_value());

	// An empty group is not a group
	{
		parity_shard empty = p;
		empty.blob_size.clear();
		CHECK(not fec::reconstruct(empty, lookup_without(f, {})).has_value());
	}

	// A length table that contradicts the shards it names: a corrupt or
	// version-skewed datagram, refused rather than decoded into garbage
	{
		parity_shard corrupt = p;
		corrupt.blob_size[1] += 1;
		const size_t dropped = corrupt.first_shard_idx + 3;
		CHECK(not fec::reconstruct(corrupt, lookup_without(f, std::span(&dropped, 1))).has_value());
	}

	// A length longer than the parity payload itself cannot name any real shard
	{
		parity_shard corrupt = p;
		corrupt.blob_size[2] = uint16_t(corrupt.payload.size() + 1);
		const size_t dropped = corrupt.first_shard_idx + 2;
		CHECK(not fec::reconstruct(corrupt, lookup_without(f, std::span(&dropped, 1))).has_value());
	}

	// A payload that has been truncated on the wire: the XOR would run past its end
	{
		parity_shard corrupt = p;
		corrupt.payload = corrupt.payload.first(corrupt.payload.size() / 2);
		const size_t dropped = corrupt.first_shard_idx + 1;
		CHECK(not fec::reconstruct(corrupt, lookup_without(f, std::span(&dropped, 1))).has_value());
	}

	// Blob bytes that decode to nothing sensible: refused, never thrown out of the
	// network thread
	{
		frame junk = make_frame(8 * 1024);
		std::vector<uint8_t> & payload = junk.parity_payloads.front();
		for (uint8_t & b: payload)
			b = 0xff;
		junk.parity.front().payload = payload;
		const size_t dropped = 2;
		auto shard = fec::reconstruct(junk.parity.front(), lookup_without(junk, std::span(&dropped, 1)));
		// Either it refuses or it produces something; what matters is that it
		// returns rather than throwing, and that it never claims the real shard
		CHECK(not shard or not same_shard(*shard, junk.shards[2]));
	}
}

void test_dedup()
{
	std::printf("Part D: a rebuilt shard is an ordinary shard\n");

	frame f = make_frame(32 * 1024);

	// The accumulator's rule, verbatim: first one in wins, later copies are dropped
	std::vector<std::optional<data_shard>> slots(f.shards.size());
	auto insert = [&slots](data_shard && s) -> bool {
		if (slots[s.shard_idx])
			return false;
		slots[s.shard_idx] = std::move(s);
		return true;
	};

	const size_t lost = 5;
	for (size_t i = 0; i < f.shards.size(); ++i)
		if (i != lost)
			insert(data_shard(f.shards[i]));

	auto rebuilt = fec::reconstruct(f.parity.front(), [&slots](uint16_t idx) -> const data_shard * {
		return (idx < slots.size() and slots[idx]) ? &*slots[idx] : nullptr;
	});
	CHECK(rebuilt.has_value());
	CHECK(rebuilt and insert(std::move(*rebuilt)));
	CHECK(slots[lost].has_value());
	CHECK(slots[lost] and same_shard(*slots[lost], f.shards[lost]));

	// The real copy turns up late — a duplicate, dropped, and the frame is the same
	// either way because the rebuilt bytes are the real bytes
	std::vector<uint8_t> before(slots[lost]->payload.begin(), slots[lost]->payload.end());
	CHECK(not insert(data_shard(f.shards[lost])));
	std::vector<uint8_t> after(slots[lost]->payload.begin(), slots[lost]->payload.end());
	CHECK(before == after);
	CHECK(before.size() == f.shards[lost].payload.size());
	CHECK(std::memcmp(before.data(), f.shards[lost].payload.data(), before.size()) == 0);

	// The frame is whole: every slot filled, the last one carrying the timings, so
	// the accumulator submits it and never counts it lost
	bool whole = true;
	for (const auto & s: slots)
		whole = whole and s.has_value();
	CHECK(whole);
	CHECK(slots.back()->timing_info.has_value());
	CHECK(slots.front()->view_info.has_value());

	// Reconstructing again once the group is whole does nothing, so a parity shard
	// that is retried cannot double count
	CHECK(not fec::reconstruct(f.parity.front(), [&slots](uint16_t idx) -> const data_shard * {
		          return (idx < slots.size() and slots[idx]) ? &*slots[idx] : nullptr;
	          }).has_value());
}

// The headset's repair loop, as shard_accumulator::drain_parity runs it: every parity
// still held is retried until a whole pass rebuilds nothing, because filling one hole
// can be what puts another group's parity back in reach. Returns the shards still
// missing at the end, and checks that everything it did rebuild is byte for byte the
// shard that was lost.
size_t repair(const frame & f, std::span<const size_t> dropped)
{
	std::vector<std::optional<data_shard>> slots(f.shards.size());
	for (size_t i = 0; i < f.shards.size(); ++i)
	{
		bool gone = false;
		for (size_t d: dropped)
			gone = gone or d == i;
		if (not gone)
			slots[i] = f.shards[i];
	}

	auto present = [&slots](uint16_t idx) -> const data_shard * {
		return (idx < slots.size() and slots[idx]) ? &*slots[idx] : nullptr;
	};

	for (bool progress = true; progress;)
	{
		progress = false;
		for (const parity_shard & p: f.parity)
		{
			auto s = fec::reconstruct(p, present);
			if (not s or s->shard_idx >= slots.size() or slots[s->shard_idx])
				continue;
			CHECK(same_shard(*s, f.shards[s->shard_idx]));
			slots[s->shard_idx] = std::move(*s);
			progress = true;
		}
	}

	size_t missing = 0;
	for (const auto & s: slots)
		missing += s ? 0 : 1;
	return missing;
}

void test_variable_group_sizes()
{
	std::printf("Part E: the ratios the adaptive scheme picks between\n");

	for (uint16_t k: {fec::heavy_group_size, fec::moderate_group_size, fec::clean_group_size})
	{
		frame f = make_frame(200 * 1024, 0, 42, k, 1);

		// Every shard covered exactly once, in order, no gap and no overlap
		size_t expected_first = 0;
		for (const parity_shard & p: f.parity)
		{
			CHECK(p.first_shard_idx == expected_first);
			CHECK(p.shard_stride == 1);
			CHECK(not p.blob_size.empty());
			CHECK(p.blob_size.size() <= k);
			if (&p != &f.parity.back())
				CHECK(p.blob_size.size() == k);
			expected_first += p.blob_size.size();
		}
		CHECK(expected_first == f.shards.size());
		CHECK(f.parity.size() == (f.shards.size() + k - 1) / k);

		// The overhead the bitrate accounting is scaled by: 1/(K+1) of the whole,
		// i.e. 1/K of the data. fec::data_share is the other side of that sum.
		size_t data_bytes = 0;
		for (const data_shard & s: f.shards)
			data_bytes += serialized_size(s);
		size_t parity_bytes = 0;
		for (const parity_shard & p: f.parity)
			parity_bytes += serialized_size(p);
		const double overhead = double(parity_bytes) / data_bytes;
		const double nominal = 1.0 / k;
		CHECK(overhead > nominal * 0.85 and overhead < nominal * 1.2);
		CHECK(fec::data_share(k) > 1.0 / (1.0 + nominal * 1.2));

		// And the property payload_reserve exists for, at every K: a parity
		// datagram is never bigger than the data datagram the same encoder would
		// have produced with FEC off. A group of 16 needs a longer length table
		// than a group of 8, which is exactly why the reserve is not a constant.
		size_t largest_parity = 0;
		for (const parity_shard & p: f.parity)
			largest_parity = std::max(largest_parity, serialized_size(p));
		size_t largest_data = 0;
		for (const data_shard & s: f.shards)
			largest_data = std::max(largest_data, serialized_size(s));
		CHECK(largest_parity <= largest_data + fec::payload_reserve(k));
		CHECK(largest_parity <= data_shard::max_payload_size + fec::payload_reserve(k));

		// Every position of every group still round trips, including the two the
		// frame cannot do without
		size_t rebuilt = 0;
		for (const parity_shard & original: f.parity)
		{
			parity_shard p = round_trip(original);
			CHECK(p.shard_stride == original.shard_stride);
			for (size_t i = 0; i < p.blob_size.size(); ++i)
			{
				const size_t dropped = p.first_shard_idx + i;
				auto shard = fec::reconstruct(p, lookup_without(f, std::span(&dropped, 1)));
				CHECK(shard.has_value());
				if (shard)
				{
					++rebuilt;
					CHECK(same_shard(*shard, f.shards[dropped]));
				}
			}
		}
		CHECK(rebuilt == f.shards.size());

		std::printf("  %u+1: %zu shards, %zu parity, %.1f%% overhead, reserve %zu B\n",
		            unsigned(k),
		            f.shards.size(),
		            f.parity.size(),
		            overhead * 100,
		            fec::payload_reserve(k));
	}

	// The payload budget shrinks as the group grows, which is what keeps the parity
	// datagram inside a data one, and it is unchanged for the ratio that was fixed
	CHECK(fec::payload_reserve(fec::group_size) == 64);
	CHECK(fec::shard_payload_budget(true, fec::clean_group_size) <
	      fec::shard_payload_budget(true, fec::heavy_group_size));
	CHECK(fec::shard_payload_budget(false, fec::clean_group_size) == data_shard::max_payload_size);
}

void test_interleaving()
{
	std::printf("Part F: striding, and the bursts it is for\n");

	const uint16_t k = fec::group_size;
	const uint16_t d = fec::interleave_depth;
	frame strided = make_frame(200 * 1024, 0, 42, k, d);
	frame contiguous = make_frame(200 * 1024, 0, 42, k, 1);
	CHECK(strided.shards.size() == contiguous.shards.size());

	// Same protection budget either way: striding changes which shards a parity
	// covers, not how many parities there are
	CHECK(strided.parity.size() == contiguous.parity.size());

	// The groups of one block are strided by the interleave depth, and together the
	// parities still cover every shard exactly once
	std::vector<int> covered(strided.shards.size(), 0);
	for (const parity_shard & p: strided.parity)
	{
		CHECK(p.shard_stride == d);
		for (size_t i = 0; i < p.blob_size.size(); ++i)
		{
			const size_t idx = size_t(p.first_shard_idx) + i * p.shard_stride;
			CHECK(idx < covered.size());
			if (idx < covered.size())
				++covered[idx];
		}
	}
	for (int c: covered)
		CHECK(c == 1);

	// The point of the whole thing. A run of consecutive datagrams goes into the air
	// together and is lost together; strided, each of them is the only hole in its own
	// group and every one comes back.
	for (size_t burst = 2; burst <= d; ++burst)
	{
		// Start somewhere inside a block rather than on its boundary, which is the
		// hard case: the run then spans two groups' worth of positions
		for (size_t start: {size_t(5), size_t(17), size_t(33)})
		{
			std::vector<size_t> dropped;
			for (size_t i = 0; i < burst; ++i)
				dropped.push_back(start + i);

			CHECK(repair(strided, dropped) == 0);
			// And the same burst against contiguous groups is not repairable:
			// two or more of the lost shards share a group, and a single parity
			// recovers one erasure and no more.
			CHECK(repair(contiguous, dropped) > 0);
		}
	}
	std::printf("  bursts of 2..%u recovered strided, none of them contiguous\n", unsigned(d));

	// A burst one longer than the stride is out of reach either way — the honest
	// bound, and the reason the depth is a tuning knob rather than a promise
	{
		std::vector<size_t> dropped;
		for (size_t i = 0; i <= d; ++i)
			dropped.push_back(9 + i);
		CHECK(repair(strided, dropped) > 0);
	}

	// Scattered single losses, which is the loss the contiguous scheme was sized for,
	// are still repaired with striding on
	{
		std::vector<size_t> dropped = {1, 11, 26, 40, 57};
		CHECK(repair(strided, dropped) == 0);
		CHECK(repair(contiguous, dropped) == 0);
	}

	// The first and last shards of the frame, whose loss is otherwise fatal, come back
	// out of a strided group like any other
	{
		std::vector<size_t> dropped = {0};
		CHECK(repair(strided, dropped) == 0);
	}
	{
		std::vector<size_t> dropped = {strided.shards.size() - 1};
		CHECK(repair(strided, dropped) == 0);
	}

	// A frame shorter than one block: the last block is partial and every group of it
	// is short, which must not change any of the above
	{
		frame small = make_frame(fec::shard_payload_budget(true, k) * 6, 0, 7, k, d);
		CHECK(small.shards.size() >= 6);
		CHECK(small.parity.size() == std::min<size_t>(d, small.shards.size()));
		std::vector<size_t> dropped = {2, 3};
		CHECK(repair(small, dropped) == 0);
	}

	// A stride of 0 never comes off the builder, and a corrupt one that arrives must
	// be read as the contiguous 1 rather than dividing by it or spinning
	{
		parity_shard corrupt = contiguous.parity.front();
		corrupt.shard_stride = 0;
		const size_t dropped = 2;
		auto shard = fec::reconstruct(corrupt, lookup_without(contiguous, std::span(&dropped, 1)));
		CHECK(shard.has_value());
		CHECK(shard and same_shard(*shard, contiguous.shards[2]));
	}

	// A stride that runs the group off the end of the index space is refused rather
	// than wrapped into some other frame's shards
	{
		parity_shard corrupt = strided.parity.front();
		corrupt.first_shard_idx = 65000;
		corrupt.shard_stride = 4096;
		const size_t dropped = 0;
		CHECK(not fec::reconstruct(corrupt, lookup_without(strided, std::span(&dropped, 1))).has_value());
	}
}

void test_rate_controller()
{
	std::printf("Part G: choosing the ratio from the loss\n");

	fec::rate_controller rc;
	CHECK(rc.group_size() == fec::moderate_group_size);

	// A clean link settles on the cheap ratio, but not instantly: the way down is the
	// slow one, and it is the dwell that makes it so.
	for (unsigned i = 0; i < fec::rate_controller::relax_frames - 1; ++i)
		rc.on_frame(200, 0, true);
	CHECK(rc.group_size() == fec::moderate_group_size);
	rc.on_frame(200, 0, true);
	CHECK(rc.group_size() == fec::clean_group_size);
	CHECK(rc.loss_rate() < fec::rate_controller::moderate_off);

	// Loss arrives. Tightening happens the moment the measure crosses, with no dwell
	// to wait out: one frame of 5% loss is already enough to leave the cheap ratio,
	// and the attack is fast enough that sustained loss reaches the most protective
	// one within a couple of frames — about 25 ms at 90 Hz.
	rc.on_frame(200, 10, true); // 5%, well past the heavy threshold
	CHECK(rc.group_size() == fec::moderate_group_size);
	rc.on_frame(200, 10, true);
	CHECK(rc.group_size() == fec::heavy_group_size);
	CHECK(rc.loss_rate() > fec::rate_controller::heavy_on);

	// A frame that never reached the decoder counts as heavy loss whatever its shard
	// count says: FEC and retransmission together did not save it.
	{
		fec::rate_controller c;
		for (unsigned i = 0; i < fec::rate_controller::relax_frames; ++i)
			c.on_frame(200, 0, true);
		CHECK(c.group_size() == fec::clean_group_size);
		c.on_frame(200, 0, false);
		CHECK(c.group_size() < fec::clean_group_size);
	}

	// Sitting exactly on a threshold must not flap the ratio, because a ratio change
	// is a bitrate change and a bitrate that oscillates is worse than either value.
	{
		fec::rate_controller c;
		// 0.5% of loss: past the moderate threshold, nowhere near the heavy one
		for (unsigned i = 0; i < 400; ++i)
			c.on_frame(200, 1, true);
		CHECK(c.group_size() == fec::moderate_group_size);

		uint16_t seen = c.group_size();
		unsigned changes = 0;
		for (unsigned i = 0; i < 600; ++i)
		{
			// Alternating either side of the threshold, which is what a link
			// hovering there looks like
			c.on_frame(200, i % 2 ? 1 : 0, true);
			if (c.group_size() != seen)
			{
				++changes;
				seen = c.group_size();
			}
		}
		CHECK(changes == 0);
		CHECK(c.group_size() == fec::moderate_group_size);
	}

	// And the way back up from heavy loss is one step at a time, never straight to the
	// cheapest ratio
	{
		fec::rate_controller c;
		c.on_frame(200, 20, true);
		CHECK(c.group_size() == fec::heavy_group_size);
		for (unsigned i = 0; i < fec::rate_controller::relax_frames * 3; ++i)
			c.on_frame(200, 0, true);
		// Two dwells of clean frames is enough for two steps, and no more than two
		CHECK(c.group_size() == fec::clean_group_size);

		// Half that is one step and no more. The dwell only counts frames the measure
		// has already fallen under the off threshold for, so a step costs the decay
		// plus the dwell, not the dwell alone — which is the point: the way down is
		// slow on purpose.
		fec::rate_controller c2;
		c2.on_frame(200, 20, true);
		for (unsigned i = 0; i < fec::rate_controller::relax_frames * 2; ++i)
			c2.on_frame(200, 0, true);
		CHECK(c2.group_size() == fec::moderate_group_size);
	}

	// A frame nothing is known about says nothing
	{
		fec::rate_controller c;
		const double before = c.loss_rate();
		c.on_frame(0, 0, true);
		CHECK(c.loss_rate() == before);
		CHECK(c.group_size() == fec::moderate_group_size);
	}

	// More loss reported than shards sent is a nonsense the headset could still send;
	// it must saturate rather than run the ratio off the end
	{
		fec::rate_controller c;
		c.on_frame(10, 1000, true);
		CHECK(c.loss_rate() <= 1.0);
		CHECK(c.group_size() == fec::heavy_group_size);
	}

	// Turning the switch off puts the ratio back where the fixed scheme has it
	{
		fec::rate_controller c;
		c.on_frame(200, 20, true);
		CHECK(c.group_size() == fec::heavy_group_size);
		c.reset();
		CHECK(c.group_size() == fec::group_size);
		CHECK(c.loss_rate() == 0);
	}
}

} // namespace

int main()
{
	test_group_construction();
	test_round_trip();
	test_recovery_blob_survives_send_mutation();
	test_span_xor_matches_encoded_blob();
	test_reused_blob_metadata_threads();
	test_malformed_bool_deserialization();
	test_graceful_failure();
	test_dedup();
	test_variable_group_sizes();
	test_interleaving();
	test_rate_controller();

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
