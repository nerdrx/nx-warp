#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace wivrn::nxastc_packet
{
inline constexpr uint32_t compact_single_mode = 0x100f3;
inline constexpr uint32_t compact_dual_mode = 0x10442;

inline constexpr uint64_t compact_block_bytes(uint32_t width, uint32_t height)
{
	return ((uint64_t(width) + 7) / 8) * ((uint64_t(height) + 7) / 8) * 14;
}

inline bool compact_bytes_for_raw(size_t raw_bytes, size_t & compact_bytes) noexcept
{
	if (raw_bytes % 16 != 0)
		return false;
	const size_t blocks = raw_bytes / 16;
	if (blocks > std::numeric_limits<size_t>::max() / 14)
		return false;
	compact_bytes = blocks * 14;
	return true;
}

inline uint32_t compact_mode(const uint8_t * block) noexcept
{
	return uint32_t(block[0]) | (uint32_t(block[1]) << 8) | (uint32_t(block[2] & 1u) << 16);
}

inline bool compact_mode_known(uint32_t mode) noexcept
{
	return mode == compact_single_mode || mode == compact_dual_mode;
}

// Remove the fixed 17-bit header from each known 8x8 ASTC block.
inline bool compact_blocks(std::span<const uint8_t> raw, std::span<uint8_t> compact) noexcept
{
	size_t expected;
	if (!compact_bytes_for_raw(raw.size(), expected) || compact.size() != expected)
		return false;
	for (size_t i = 0; i < raw.size(); i += 16)
		if (!compact_mode_known(compact_mode(raw.data() + i)))
			return false;
	for (size_t i = 0; i < raw.size(); i += 16)
	{
		const uint8_t * src = raw.data() + i;
		uint8_t * dst = compact.data() + (i / 16) * 14;
		const uint32_t mode = compact_mode(src);
		dst[0] = uint8_t((src[2] & 0xfeu) | (mode == compact_dual_mode ? 1u : 0u));
		std::memcpy(dst + 1, src + 3, 13);
	}
	return true;
}

// Restore compact blocks in-place in a raw-sized output buffer. Process backwards
// because each expanded block is two bytes larger than its compact source.
inline bool expand_compact_blocks(std::span<uint8_t> raw_buffer, size_t compact_bytes) noexcept
{
	size_t expected;
	if (!compact_bytes_for_raw(raw_buffer.size(), expected) || compact_bytes != expected)
		return false;
	const size_t blocks = compact_bytes / 14;
	for (size_t remaining = blocks; remaining != 0;)
	{
		const size_t i = --remaining;
		uint8_t * src = raw_buffer.data() + i * 14;
		uint8_t * dst = raw_buffer.data() + i * 16;
		const uint8_t first = src[0];
		const uint32_t mode = (first & 1u) ? compact_dual_mode : compact_single_mode;
		std::memmove(dst + 3, src + 1, 13);
		dst[0] = uint8_t(mode);
		dst[1] = uint8_t(mode >> 8);
		dst[2] = uint8_t(((mode >> 16) & 1u) | (first & 0xfeu));
	}
	return true;
}
} // namespace wivrn::nxastc_packet
