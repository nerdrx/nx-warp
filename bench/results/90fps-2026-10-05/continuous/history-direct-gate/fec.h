/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn NX contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Forward error correction for the video stream.
//
// Both ends need the same two things and nothing else: the exact bytes a data
// shard contributes to the XOR (encode_blob), and the inverse (decode_blob). The
// server also uses group_builder to accumulate a group and hand out its parity
// shard; the headset needs no equivalent because a parity shard describes its own
// group. tests/fec_test.cpp drives all of it without a socket.
namespace wivrn::fec
{

using data_shard = to_headset::video_stream_data_shard;
using parity_shard = to_headset::video_stream_parity_shard;

// Data shards one parity shard covers, with the adaptive ratio off. Eight data
// shards plus one parity is 12.5% on the wire, and a group is then about 11 kB —
// near enough the 12 kB micro-burst the packet pacer hands to the kernel between
// two sleeps that a group and a burst are the same order of thing, without being
// locked to each other: the two sizes drift against one another over a frame, which
// is what scatters the parity shards across the bursts instead of parking them all
// at one boundary.
//
// Not a protocol constant: the parity shard names the shards it covers, so the
// server may change this at will and old headsets keep up.
inline constexpr uint16_t group_size = 8;

// The three ratios the adaptive controller picks between, from cheapest to most
// protective. 16+1 is 6% on the wire, 8+1 is 12.5%, 4+1 is 25%.
inline constexpr uint16_t clean_group_size = 16;
inline constexpr uint16_t moderate_group_size = 8;
inline constexpr uint16_t heavy_group_size = 4;

// How many groups are open at once while the adaptive scheme is on, i.e. the stride
// between the shards one parity covers.
//
// A plain contiguous group loses everything the moment two of its own shards go in
// the same burst, and a Wi-Fi hiccup drops a run of datagrams far more often than it
// drops two independent ones. Striding the assignment — shard i joins group i mod D —
// spreads any run of up to D consecutive datagrams over D different groups, one
// erasure each, which is exactly what a single-parity XOR repairs.
//
// D is deliberately not tied to the group size: a block is K*D shards long and has to
// stay well inside a frame, or every parity of the frame would end up in one tail
// burst — the very thing send_parity() emits per group to avoid. Four is a burst
// tolerance worth having (4 * 1.4 kB = 5.6 kB of contiguous loss) at a block of 32-64
// shards, i.e. 45-90 kB, which is a frame or less at the bitrates this runs at.
inline constexpr uint16_t interleave_depth = 4;

// Fraction of the on-wire video budget that carries actual video while FEC is on.
// The encoder bitrate is scaled by this so the total, parity included, stays at
// the number the bitrate controller decided. Varies with the group size, so the
// adaptive controller has to re-derive the encoder bitrate whenever it moves.
inline constexpr double data_share(uint16_t k)
{
	return double(k) / (k + 1);
}

// Bytes taken out of a data shard's payload budget while FEC is on.
//
// A parity shard must not be a bigger datagram than a data shard, or FEC would
// start causing the fragmentation it exists to survive. It is bigger by three
// things: the per-shard length table (2 bytes of length prefix plus 2 per covered
// shard, 18 for a group of 8), the recovery blob's own framing (the empty
// timing_info flag and the payload length prefix that a data shard does not repeat,
// 3 bytes), and the timing_info the last shard of a frame carries, which lands in
// the longest blob of whichever group holds it (33 bytes). That is 54 for a group of
// 8; the ten bytes on top leave room for a timing_info that grows a field.
//
// Scales with the group size because the length table does: a group of 16 needs 80,
// a group of 4 needs 56. A group of 8 comes out at exactly the 64 this was a constant
// at before the ratio became adaptive.
//
// The cost is 4.6% more datagrams per frame for the same bytes, i.e. 4.6% more
// UDP/IP headers — under 0.2% of the link. group_builder additionally refuses to
// emit a parity shard whose payload would exceed max_payload_size, so an unforeseen
// blob can only cost the group its protection, never fragment a datagram.
inline constexpr size_t payload_reserve(uint16_t k)
{
	return 2 + 2 * size_t(k) + 3 + 33 + 10;
}

// Payload budget of a data shard, before the first shard's view_info is taken out
// of it, with and without FEC.
inline constexpr size_t shard_payload_budget(bool fec_enabled, uint16_t k = group_size)
{
	return data_shard::max_payload_size - (fec_enabled ? payload_reserve(k) : 0);
}

// Shard indices the group of `parity` covers: first_shard_idx, then every
// shard_stride past it. A stride of 0 is a corrupt datagram; treated as the
// contiguous 1 so the arithmetic below cannot degenerate.
inline uint16_t group_stride(const parity_shard & parity)
{
	return parity.shard_stride ? parity.shard_stride : 1;
}

// One past the highest shard index the group of `parity` covers, as a size_t so an
// index table that would run off the end of a uint16 is visible rather than wrapped.
inline size_t group_end(const parity_shard & parity)
{
	const size_t n = parity.blob_size.size();
	if (n == 0)
		return parity.first_shard_idx;
	return size_t(parity.first_shard_idx) + (n - 1) * group_stride(parity) + 1;
}

// The part of a data shard that a parity shard can rebuild: everything except the
// three fields (stream, frame, shard index) the parity shard already knows.
//
// Encoded exactly as the data shard encodes them, so a blob is the tail of a data
// shard's own serialization with the payload length made explicit. Appends to
// `out`, which is cleared first.
inline std::vector<std::span<uint8_t>> & encode_blob_spans(const data_shard & shard)
{
	// Like socket sends, keep bounded metadata storage local to each thread.
	thread_local serialization_packet p;
	p.clear();
	p.serialize(shard.view_info);
	p.serialize(shard.timing_info);
	p.serialize(shard.payload);
	return static_cast<std::vector<std::span<uint8_t>> &>(p);
}


inline void encode_blob(const data_shard & shard, std::vector<uint8_t> & out)
{
	const auto & spans = encode_blob_spans(shard);
	out.clear();
	for (const std::span<uint8_t> & span: spans)
		out.insert(out.end(), span.begin(), span.end());
}

// XOR the exact encode_blob bytes without materializing a second full blob.
// Keep serialization centralized so field order and wire representation cannot drift.
inline bool xor_blob_into(const data_shard & shard, std::span<uint8_t> recovered, size_t expected_size)
{
	thread_local serialization_packet packet;
	packet.clear();
	packet.serialize(shard.view_info);
	packet.serialize(shard.timing_info);
	packet.serialize(shard.payload);
	auto & spans = static_cast<std::vector<std::span<uint8_t>> &>(packet);

	size_t size = 0;
	for (const auto span: spans)
	{
		if (span.size() > recovered.size() - std::min(recovered.size(), size))
			return false;
		size += span.size();
	}
	if (size != expected_size)
		return false;

	size_t offset = 0;
	for (const auto span: spans)
	{
		for (size_t i = 0; i < span.size(); ++i)
			recovered[offset + i] ^= span[i];
		offset += span.size();
	}
	return true;
}

// Inverse of encode_blob. Throws deserialization_error on a blob that does not
// decode; callers treat that as "no reconstruction" rather than as a fatal error,
// since a corrupt or mismatched parity shard must never take the session down.
//
// The returned shard owns a copy of the blob: its payload span points into it and
// stays valid for as long as the shard does, exactly like a received one.
inline data_shard decode_blob(uint8_t stream_item_idx,
                              uint64_t frame_idx,
                              uint16_t shard_idx,
                              std::span<const uint8_t> blob)
{
	auto memory = std::make_shared_for_overwrite<uint8_t[]>(blob.size() + 1);
	std::copy(blob.begin(), blob.end(), memory.get());
	deserialization_packet p{memory, std::span<uint8_t>(memory.get(), blob.size())};

	data_shard shard;
	shard.stream_item_idx = stream_item_idx;
	shard.frame_idx = frame_idx;
	shard.shard_idx = shard_idx;
	shard.view_info = p.deserialize<std::optional<data_shard::view_info_t>>();
	shard.timing_info = p.deserialize<std::optional<data_shard::timing_info_t>>();
	shard.payload = p.deserialize<std::span<uint8_t>>();
	shard.data.c = p.steal_buffer();
	return shard;
}

// Rebuild the one data shard of `parity`'s group that `present` does not hold.
//
// `present` is looked up by shard index and returns null for a shard that has not
// arrived; exactly one of the covered indices must return null. Returns nothing
// when that is not the case (nothing missing, or more than one — XOR recovers a
// single erasure and no more), or when the group's blobs contradict the lengths
// the parity shard recorded, which is what a corrupt datagram looks like.
//
// The covered indices are first_shard_idx, first_shard_idx + shard_stride, and so
// on: a stride of 1 is the contiguous group, anything more is the interleaved
// scheme, and the shard being rebuilt is found the same way either way.
template <typename lookup>
std::optional<data_shard> reconstruct(const parity_shard & parity, lookup && present)
{
	const size_t n = parity.blob_size.size();
	if (n == 0)
		return {};

	const uint16_t stride = group_stride(parity);
	if (group_end(parity) > std::numeric_limits<uint16_t>::max())
		return {}; // corrupt index table, the group runs off the end of the index space
	const auto index_of = [&](size_t i) { return uint16_t(parity.first_shard_idx + i * stride); };

	size_t missing = n;
	for (size_t i = 0; i < n; ++i)
	{
		if (present(index_of(i)))
			continue;
		if (missing != n)
			return {}; // two or more erasures, out of reach of a single parity
		missing = i;
	}
	if (missing == n)
		return {}; // the group is whole, nothing to do

	// Undo the XOR with everything that did arrive. The parity payload is as long
	// as the longest blob of the group and every blob was zero padded to it, so a
	// present blob can never be longer than it.
	std::vector<uint8_t> recovered(parity.payload.begin(), parity.payload.end());
	for (size_t i = 0; i < n; ++i)
	{
		if (i == missing)
			continue;

		const data_shard * shard = present(index_of(i));
		if (not xor_blob_into(*shard, recovered, parity.blob_size[i]))
			return {};
	}

	const size_t length = parity.blob_size[missing];
	if (length > recovered.size())
		return {};

	try
	{
		return decode_blob(parity.stream_item_idx,
		                   parity.frame_idx,
		                   index_of(missing),
		                   std::span<const uint8_t>(recovered).first(length));
	}
	catch (...)
	{
		return {};
	}
}

// Server side: accumulates the recovery blobs of a block of shards and hands out one
// parity shard per group of it.
//
// A block is `group_size() * depth()` shards; within it, shard number j joins group
// j mod depth, so a group covers indices block_first + g, + g + depth, + g + 2*depth
// and so on — which is the shard_stride the parity shard carries. At depth 1 that is
// the contiguous scheme this had before interleaving, one group open at a time.
//
// Shards are added in the order they go out. A shard whose index does not continue
// the open block starts a new one — dropping whatever protection the open block had
// accumulated — which is what keeps the index arithmetic honest when a frame's shards
// do not all travel the same way (a parameter set going out on the control socket in
// the middle of a frame, say).
//
// Usage per block: add() until block_full(), then take() repeatedly until it returns
// nothing, which is also what opens the next block. Same at the end of a frame, where
// the block is usually a partial one.
class group_builder
{
public:
	// Group size and interleave depth of the blocks this builder produces. Takes
	// effect at the next reset(), i.e. at the next frame: the shard payload budget
	// is derived from the group size too (see payload_reserve) and a frame must be
	// sharded to one size throughout.
	void set_layout(uint16_t group_size_, uint16_t depth_)
	{
		pending_k = std::max<uint16_t>(1, group_size_);
		pending_d = std::max<uint16_t>(1, depth_);
	}

	uint16_t group_size() const
	{
		return k;
	}

	uint16_t depth() const
	{
		return d;
	}

	void reset(uint8_t stream_item_idx, uint64_t frame_idx)
	{
		stream = stream_item_idx;
		frame = frame_idx;
		k = pending_k;
		d = pending_d;
		groups.resize(d);
		start_block(0);
	}

	bool empty() const
	{
		return count == 0;
	}

	// The block has all of its `group_size() * depth()` shards: every group of it is
	// as full as it is going to get and its parity shards are due.
	bool block_full() const
	{
		return count >= size_t(k) * d;
	}

	// Borrows the scratch blob; take() preserves it. Copy before the next add(),
	// reset(), or destruction if the bytes need to outlive that operation.
	std::span<const uint8_t> add(const data_shard & shard, bool on_primary = true)
	{
		encode_blob(shard, scratch);
		return add_blob(shard, scratch, on_primary);
	}

	// Capture recovery bytes before sending: socket encryption may mutate borrowed
	// payload spans in place, so encoding the shard after send can put ciphertext
	// into parity and retransmission history.
	std::span<const uint8_t> add_blob(const data_shard & shard, std::span<const uint8_t> blob, bool on_primary = true)
	{
		// A block that was left half drained, or one this shard does not continue,
		// is over: the indices a parity would name would not be the ones it covers.
		if (count and (cursor != 0 or shard.shard_idx != uint16_t(block_first + count)))
			start_block(shard.shard_idx);
		if (count == 0)
			start_block(shard.shard_idx);

		group & g = groups[count % d];

		if (g.parity.size() < blob.size())
			g.parity.resize(blob.size(), 0);
		for (size_t i = 0; i < blob.size(); ++i)
			g.parity[i] ^= blob[i];

		g.sizes.push_back(uint16_t(blob.size()));
		g.on_primary = g.on_primary or on_primary;
		++count;
		return blob;
	}

	// Next parity shard the open block still owes, or nothing once it owes none —
	// which is also when the next block is opened. Groups that took no shard, groups
	// every shard of which went over a path that cannot lose one, and groups whose
	// blobs came out longer than a datagram may carry (see payload_reserve) are
	// skipped rather than returned.
	//
	// The returned shard's payload points into a buffer of this builder that stays
	// untouched until the next take(), which is all the sender needs: it serializes
	// the shard into the socket before it asks for the next one.
	std::optional<parity_shard> take()
	{
		while (cursor < d)
		{
			group & g = groups[cursor];
			const uint16_t idx = cursor;
			++cursor;

			if (g.sizes.empty() or not g.on_primary)
				continue;

			out.swap(g.parity);
			if (out.size() > data_shard::max_payload_size)
				continue;

			return parity_shard{
			        .stream_item_idx = stream,
			        .frame_idx = frame,
			        .first_shard_idx = uint16_t(block_first + idx),
			        .shard_stride = d,
			        .blob_size = g.sizes,
			        .payload = out,
			        .data = {},
			};
		}

		start_block(uint16_t(block_first + count));
		return {};
	}

private:
	struct group
	{
		std::vector<uint16_t> sizes;
		std::vector<uint8_t> parity;
		// Whether any shard of this group went out on a path that can drop it. A
		// group that spilled whole to the secondary (TCP) path would have its
		// parity repair nothing while still costing Wi-Fi bandwidth.
		bool on_primary = false;
	};

	void start_block(uint16_t next_first)
	{
		block_first = next_first;
		count = 0;
		cursor = 0;
		for (group & g: groups)
		{
			g.sizes.clear();
			g.parity.clear();
			g.on_primary = false;
		}
	}

	uint8_t stream = 0;
	uint64_t frame = 0;
	uint16_t k = ::wivrn::fec::group_size;
	uint16_t d = 1;
	uint16_t pending_k = ::wivrn::fec::group_size;
	uint16_t pending_d = 1;
	uint16_t block_first = 0;
	// Shards in the open block, and which group take() looks at next
	uint16_t count = 0;
	uint16_t cursor = 0;
	std::vector<group> groups{1};
	std::vector<uint8_t> out;
	std::vector<uint8_t> scratch;
};

// Picks the parity group size from the loss the headset reports.
//
// The signal is one number per frame: the fraction of that frame's data shards the
// headset had to rebuild from parity or ask for again, floored at `incomplete_loss`
// for a frame that never reached the decoder at all — whatever the count says, a
// frame that FEC and retransmission together could not save is evidence that the
// protection is too thin.
//
// The response is deliberately lopsided. Tightening is immediate: the first frame
// whose measurement crosses a threshold moves the ratio, because the loss is already
// costing frames. Relaxing is slow, one step at a time, and only after the measure
// has stayed *well* clear of the threshold that got us here for `relax_frames` in a
// row — two separate brakes (a gap between the on and off thresholds, and a dwell)
// so that a link sitting near a threshold cannot flap the ratio, which would flap the
// encoder bitrate with it.
class rate_controller
{
public:
	// Loss below this is a clean link and buys the cheapest ratio; above the heavy
	// threshold, the most protective one. The off thresholds are where the ratio is
	// allowed back down again.
	static constexpr double heavy_on = 0.02;
	static constexpr double heavy_off = 0.010;
	static constexpr double moderate_on = 0.001;
	static constexpr double moderate_off = 0.0005;
	// A frame that never reached the decoder counts as at least this much loss
	static constexpr double incomplete_loss = 0.03;
	// Rises are followed at a quarter of the gap per frame (about 25 ms of attack at
	// 90 Hz), falls at a fiftieth (about half a second): loss has to stop, not merely
	// pause, before the measure comes back down.
	static constexpr double attack = 0.25;
	static constexpr double release = 0.02;
	// Frames the measure has to stay under an off threshold before the ratio steps
	// back down. 120 is about 1.3 s at 90 Hz.
	static constexpr unsigned relax_frames = 120;

	void reset()
	{
		ema = 0;
		clear = 0;
		k = ::wivrn::fec::moderate_group_size;
	}

	// One frame's evidence. `shards_lost` is what the headset had to rebuild or ask
	// for; `complete` is whether the frame reached the decoder at all.
	void on_frame(uint32_t shards_sent, uint32_t shards_lost, bool complete)
	{
		if (shards_sent == 0)
			return;

		double sample = double(std::min(shards_lost, shards_sent)) / shards_sent;
		if (not complete)
			sample = std::max(sample, incomplete_loss);

		ema += (sample > ema ? attack : release) * (sample - ema);

		const uint16_t want = ema > heavy_on ? heavy_group_size
		                      : ema > moderate_on
		                              ? moderate_group_size
		                              : clean_group_size;

		if (want < k)
		{
			// More loss than the current ratio is sized for: move now
			k = want;
			clear = 0;
			return;
		}
		if (want == k)
		{
			clear = 0;
			return;
		}

		const double release_below = k == heavy_group_size ? heavy_off : moderate_off;
		if (ema >= release_below)
		{
			clear = 0;
			return;
		}
		if (++clear < relax_frames)
			return;

		clear = 0;
		k = k == heavy_group_size ? moderate_group_size : clean_group_size;
	}

	uint16_t group_size() const
	{
		return k;
	}

	double loss_rate() const
	{
		return ema;
	}

private:
	double ema = 0;
	unsigned clear = 0;
	uint16_t k = moderate_group_size;
};

} // namespace wivrn::fec
