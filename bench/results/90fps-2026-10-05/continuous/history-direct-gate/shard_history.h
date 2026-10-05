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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace wivrn
{

// What one video stream has just put on the wire, kept for as long as the headset
// could still do anything about it.
//
// Two things live here because both are measurements of the same thing — what went
// out, and what did not arrive — and both are written by the sender thread and read
// by the network thread:
//
//   * The payload ring, which is what a retransmission (from_headset::nack) is served
//     out of. Only allocated while the headset asks for retransmissions; off, this
//     costs nothing at all, not even the allocation.
//   * The per-frame shard counts, always kept because they are what the adaptive
//     parity ratio measures its loss rate as a fraction of, and they are 24 bytes a
//     frame.
//
// What the ring stores per shard is its *recovery blob* — fec::encode_blob of it, i.e.
// view_info, timing_info and payload in the encoding a data shard uses — so a shard is
// put back together with fec::decode_blob and nothing else has to be remembered. It is
// deliberately the plaintext: a retransmission goes out through the ordinary send path
// and is encrypted there with a fresh IV off the global counter, because AES-CTR with a
// reused IV is not encryption at all. Storing the ciphertext and replaying it is
// exactly the mistake this avoids.
//
// Thread safe: everything that touches the ring takes the lock, and the switch itself
// is atomic so that the sender thread pays nothing at all for the feature while it is
// off. tests/nack_test.cpp drives it directly.
class shard_history
{
public:
	// Bounded plaintext recovery storage, allocated only while retransmission is on.
	// At a 1 Gbit/s stereo payload budget / 90 Hz, one eye can spend ~694 kB
	// per frame: 1 MiB already loses part of the previous frame. 2 MiB retains
	// the modeled two-frame repair horizon, including current shard metadata.
	// This is a byte bound, not a time guarantee for arbitrary payload bursts.
	static constexpr size_t capacity = 2 << 20;
	// And a hard bound on the bookkeeping, for a stream of very small shards
	static constexpr size_t max_entries = 4096;
	// Frames the shard counts go back. 32 at 90 Hz is a third of a second, far more
	// than the feedback for a frame takes to come back.
	static constexpr size_t tracked_frames = 32;

	// One shard the headset asked for, copied out of the ring so that the caller can
	// put it on the wire without holding the lock.
	struct hit
	{
		uint16_t shard_idx;
		std::vector<uint8_t> blob;
	};

	// What one frame cost, as far as the server can tell
	struct cost
	{
		uint32_t shards_sent;
		// Shards of it the headset asked to have sent again
		uint32_t shards_nacked;
	};

	// Allocate or release the payload ring. Idempotent; the headset sends its whole
	// settings block on every change.
	void set_enabled(bool enabled)
	{
		std::lock_guard lock(mutex);
		if (enabled == enabled_)
			return;
		enabled_ = enabled;
		entries.clear();
		written = 0;
		if (enabled)
			ring.resize(capacity);
		else
			ring = std::vector<uint8_t>{}; // move an empty vector to release capacity
	}

	// Atomic rather than guarded so that the sender thread can skip the lock entirely
	// on the shard path while the feature is off.
	bool enabled() const
	{
		return enabled_;
	}

	// A shard has just gone out. `blob` is fec::encode_blob of it.
	//
	// `on_primary` is false for a shard that spilled to the secondary (TCP) path,
	// which is kept out of the history on purpose: TCP does not lose it, so a request
	// naming it is the headset seeing the two paths arrive out of order, and answering
	// that with a duplicate over Wi-Fi would spend the lossy path's bandwidth on a
	// shard already in flight over the other one.
	void push(uint64_t frame_idx, uint16_t shard_idx, std::span<const uint8_t> blob, bool on_primary)
	{
		if (not enabled_ or not on_primary or blob.empty())
			return;

		std::lock_guard lock(mutex);
		if (ring.empty() or blob.size() > ring.size())
			return;

		size_t offset = written % ring.size();
		if (offset + blob.size() > ring.size())
		{
			// Never split a blob across the end of the ring: skipping the tail
			// wastes at most one shard's worth and keeps a read a plain memcpy.
			written += ring.size() - offset;
			offset = 0;
		}

		std::copy(blob.begin(), blob.end(), ring.begin() + offset);
		entries.push_back({.frame_idx = frame_idx,
		                   .shard_idx = shard_idx,
		                   .pos = written,
		                   .size = uint32_t(blob.size())});
		written += blob.size();

		// An entry's bytes are gone once the writer has come all the way round to
		// them again
		while (not entries.empty() and written - entries.front().pos > ring.size())
			entries.pop_front();
		while (entries.size() > max_entries)
			entries.pop_front();
	}

	// Scratch prototype: copy serialized spans straight into the ring while locked.
	void push_spans(uint64_t frame_idx, uint16_t shard_idx,
	                std::span<const std::span<uint8_t>> spans, bool on_primary)
	{
		if (not enabled_ or not on_primary)
			return;
		size_t size = 0;
		for (auto span: spans)
		{
			if (span.size() > capacity - size)
				return;
			size += span.size();
		}
		if (size == 0)
			return;

		std::lock_guard lock(mutex);
		if (ring.empty() or size > ring.size())
			return;
		size_t offset = written % ring.size();
		if (offset + size > ring.size())
		{
			written += ring.size() - offset;
			offset = 0;
		}
		size_t copied = 0;
		for (auto span: spans)
		{
			std::copy(span.begin(), span.end(), ring.begin() + offset + copied);
			copied += span.size();
		}
		entries.push_back({.frame_idx = frame_idx, .shard_idx = shard_idx,
		                   .pos = written, .size = uint32_t(size)});
		written += size;
		while (not entries.empty() and written - entries.front().pos > ring.size())
			entries.pop_front();
		while (entries.size() > max_entries)
			entries.pop_front();
	}

	// End of a frame: how many data shards it had. Kept whether or not the ring is on.
	void end_frame(uint64_t frame_idx, uint32_t shards)
	{
		std::lock_guard lock(mutex);
		counts_t & c = counts[frame_idx % tracked_frames];
		c = {.frame_idx = frame_idx, .shards_sent = shards, .shards_nacked = 0, .used = true};
	}

	// Shards of `frame_idx` the bitmap names that are still held, oldest index first,
	// at most `limit` of them. Bit (8 * b + i) of `bitmap`, LSB first within a byte,
	// asks for shard first_shard_idx + 8 * b + i.
	//
	// Appends to `out` and returns how many were appended. A request for a shard that
	// has aged out, that never existed, or that went over the path which cannot lose
	// it simply finds nothing — there is no error to report and nothing to answer.
	size_t collect(uint64_t frame_idx,
	               uint16_t first_shard_idx,
	               std::span<const uint8_t> bitmap,
	               size_t limit,
	               std::vector<hit> & out)
	{
		std::lock_guard lock(mutex);
		if (not enabled_ or ring.empty())
			return 0;

		size_t found = 0;
		for (const entry & e: entries)
		{
			if (found >= limit)
				break;
			if (e.frame_idx != frame_idx or e.shard_idx < first_shard_idx)
				continue;

			const size_t bit = size_t(e.shard_idx) - first_shard_idx;
			const size_t byte = bit / 8;
			if (byte >= bitmap.size())
				continue;
			if (not(bitmap[byte] & (1u << (bit % 8))))
				continue;

			const size_t offset = e.pos % ring.size();
			out.push_back({.shard_idx = e.shard_idx,
			               .blob = std::vector<uint8_t>(ring.begin() + offset,
			                                            ring.begin() + offset + e.size)});
			++found;
		}
		return found;
	}

	// Return the exact, still-cached final data shard recorded for this frame.
	// The caller checks decoded timing_info before sending it. Existing NACK hits
	// are passed to avoid returning a requested shard twice; `remaining_budget`
	// prevents an assist from taking a slot from ordinary requested data.
	std::optional<hit> collect_frame_end_candidate(uint64_t frame_idx,
	                                               std::span<const hit> already_collected,
	                                               size_t remaining_budget)
	{
		if (remaining_budget == 0)
			return {};
		auto frame = frame_cost(frame_idx);
		if (not frame or frame->shards_sent == 0 or
		    frame->shards_sent > uint32_t(std::numeric_limits<uint16_t>::max()) + 1)
			return {};

		const uint16_t last = uint16_t(frame->shards_sent - 1);
		if (std::ranges::any_of(already_collected, [last](const hit & h) { return h.shard_idx == last; }))
			return {};

		const std::array<uint8_t, 1> bitmap{1};
		std::vector<hit> candidate;
		if (collect(frame_idx, last, bitmap, 1, candidate) != 1 or candidate.front().shard_idx != last)
			return {};
		return std::move(candidate.front());
	}

	// The headset asked for `count` shards of `frame_idx`. Folded into the frame's
	// cost, which is what the adaptive parity ratio reads.
	void note_nacked(uint64_t frame_idx, uint32_t count)
	{
		std::lock_guard lock(mutex);
		counts_t & c = counts[frame_idx % tracked_frames];
		if (c.used and c.frame_idx == frame_idx)
			c.shards_nacked += count;
	}

	// What frame `frame_idx` cost, or nothing when it is no longer tracked — which is
	// the feedback for it arriving a third of a second late, i.e. never in practice.
	std::optional<cost> frame_cost(uint64_t frame_idx) const
	{
		std::lock_guard lock(mutex);
		const counts_t & c = counts[frame_idx % tracked_frames];
		if (not c.used or c.frame_idx != frame_idx)
			return {};
		return cost{.shards_sent = c.shards_sent, .shards_nacked = c.shards_nacked};
	}

	// Shards the ring is holding, for the tests and for a debug line
	size_t held() const
	{
		std::lock_guard lock(mutex);
		return entries.size();
	}

	// Bytes the ring has allocated. Zero while retransmission is off.
	size_t bytes() const
	{
		std::lock_guard lock(mutex);
		return ring.capacity();
	}

private:
	struct entry
	{
		uint64_t frame_idx;
		uint16_t shard_idx;
		// Absolute write position, never wrapped: the difference against the write
		// cursor is what says whether the bytes are still there.
		uint64_t pos;
		uint32_t size;
	};

	struct counts_t
	{
		uint64_t frame_idx = 0;
		uint32_t shards_sent = 0;
		uint32_t shards_nacked = 0;
		bool used = false;
	};

	mutable std::mutex mutex;
	std::atomic<bool> enabled_ = false;
	std::vector<uint8_t> ring;
	uint64_t written = 0;
	std::deque<entry> entries;
	std::array<counts_t, tracked_frames> counts{};
};

} // namespace wivrn
