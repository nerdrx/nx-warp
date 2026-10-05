#pragma once

#include "nxastc_packet.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace wivrn::nxastc_packet
{
inline constexpr size_t motion_reference_capacity = 16;
inline constexpr uint64_t motion_max_reference_age = 8;
inline constexpr size_t motion_selector_bytes = 1;
inline constexpr size_t motion_residual_bytes = 16;

inline constexpr bool motion_reference_usable(uint64_t current_frame, uint64_t reference_frame)
{
	return reference_frame != independent_frame && current_frame > reference_frame &&
	       current_frame - reference_frame <= motion_max_reference_age;
}

// Only successful decode reports can advance the acknowledged reference.
class motion_decode_ack
{
	std::atomic<uint64_t> newest{independent_frame};
public:
	uint64_t frame() const { return newest.load(std::memory_order_relaxed); }
	void reset() { newest.store(independent_frame, std::memory_order_relaxed); }
	void observe(uint64_t frame_index, bool decoded)
	{
		if (!decoded || frame_index == independent_frame) return;
		uint64_t old = frame();
		while ((old == independent_frame || frame_index > old) &&
		       !newest.compare_exchange_weak(old, frame_index, std::memory_order_relaxed)) {}
	}
};

namespace detail
{
inline bool motion_sizes(uint32_t width_blocks, uint32_t height_blocks, size_t & raw, size_t & packed)
{
	const uint64_t blocks = uint64_t(width_blocks) * height_blocks;
	if (!blocks || blocks > std::numeric_limits<size_t>::max() / 16 || blocks > std::numeric_limits<size_t>::max() / 17)
		return false;
	raw = size_t(blocks) * 16;
	packed = size_t(blocks) * 17;
	return true;
}

inline uint64_t load64(const uint8_t * p)
{
	uint64_t value;
	std::memcpy(&value, p, sizeof(value));
	return value;
}

inline unsigned block_distance(const uint8_t * a, const uint8_t * b)
{
	return unsigned(__builtin_popcountll(load64(a) ^ load64(b)) + __builtin_popcountll(load64(a + 8) ^ load64(b + 8)));
}

inline const uint8_t * neighbor(std::span<const uint8_t> reference,
	                               uint32_t width,
	                               uint32_t height,
	                               uint32_t x,
	                               uint32_t y,
	                               uint8_t selector)
{
	const int dx = int(selector % 3) - 1;
	const int dy = int(selector / 3) - 1;
	const uint32_t nx = dx < 0 ? (x ? x - 1 : 0) : dx > 0 ? (x + 1 < width ? x + 1 : width - 1) : x;
	const uint32_t ny = dy < 0 ? (y ? y - 1 : 0) : dy > 0 ? (y + 1 < height ? y + 1 : height - 1) : y;
	return reference.data() + (size_t(ny) * width + nx) * 16;
}
} // namespace detail

// Dimensions are ASTC block-grid dimensions. Scratch layout is selectors followed by XOR residuals.
inline bool encode_motion_blocks(uint32_t width_blocks,
	                             uint32_t height_blocks,
	                             std::span<const uint8_t> reference,
	                             std::span<const uint8_t> current,
	                             std::span<uint8_t> scratch)
{
	size_t raw = 0, packed = 0;
	if (!detail::motion_sizes(width_blocks, height_blocks, raw, packed) || reference.size() != raw || current.size() != raw || scratch.size() != packed)
		return false;
	const size_t blocks = size_t(width_blocks) * height_blocks;
	uint8_t * selectors = scratch.data();
	uint8_t * residuals = selectors + blocks;
	for (uint32_t y = 0; y < height_blocks; ++y)
		for (uint32_t x = 0; x < width_blocks; ++x)
		{
			const size_t i = size_t(y) * width_blocks + x;
			const uint8_t * cur = current.data() + i * 16;
			unsigned best = 129;
			uint8_t selected = 0;
			for (uint8_t s = 0; s < 9; ++s)
			{
				const uint8_t * candidate = detail::neighbor(reference, width_blocks, height_blocks, x, y, s);
				const unsigned distance = detail::block_distance(cur, candidate);
				if (distance < best)
				{
					best = distance;
					selected = s;
				}
			}
			selectors[i] = selected;
			const uint8_t * predictor = detail::neighbor(reference, width_blocks, height_blocks, x, y, selected);
			for (size_t j = 0; j < 16; ++j)
				residuals[i * 16 + j] = cur[j] ^ predictor[j];
		}
	return true;
}

// Validate every selector and buffer length before writing any output byte.
inline bool reconstruct_motion_blocks(uint32_t width_blocks,
	                                  uint32_t height_blocks,
	                                  std::span<const uint8_t> reference,
	                                  std::span<const uint8_t> prefix_residual,
	                                  std::span<uint8_t> output)
{
	size_t raw = 0, packed = 0;
	if (!detail::motion_sizes(width_blocks, height_blocks, raw, packed) || reference.size() != raw ||
	    prefix_residual.size() != packed || output.size() != raw)
		return false;
	const size_t blocks = size_t(width_blocks) * height_blocks;
	for (size_t i = 0; i < blocks; ++i)
		if (prefix_residual[i] > 8)
			return false;
	for (uint32_t y = 0; y < height_blocks; ++y)
	{
		const uint32_t y0 = y ? y - 1 : y;
		const uint32_t y2 = y + 1 < height_blocks ? y + 1 : y;
		const uint8_t * rows[3] = {reference.data() + size_t(y0) * width_blocks * 16,
		                           reference.data() + size_t(y) * width_blocks * 16,
		                           reference.data() + size_t(y2) * width_blocks * 16};
		for (uint32_t x = 0; x < width_blocks; ++x)
		{
			const size_t i = size_t(y) * width_blocks + x;
			const uint8_t selector = prefix_residual[i];
			const uint32_t nx = selector % 3 == 0 ? (x ? x - 1 : x) : selector % 3 == 2 ? (x + 1 < width_blocks ? x + 1 : x) : x;
			const uint8_t * predictor = rows[selector / 3] + size_t(nx) * 16;
			const uint8_t * residual = prefix_residual.data() + blocks + i * 16;
			uint8_t * dst = output.data() + i * 16;
			const uint64_t lo = detail::load64(predictor) ^ detail::load64(residual);
			const uint64_t hi = detail::load64(predictor + 8) ^ detail::load64(residual + 8);
			std::memcpy(dst, &lo, 8);
			std::memcpy(dst + 8, &hi, 8);
		}
	}
	return true;
}
} // namespace wivrn::nxastc_packet
