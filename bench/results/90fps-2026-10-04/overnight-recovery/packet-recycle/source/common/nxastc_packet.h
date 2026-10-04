#pragma once

#include "nxastc_compact.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>

namespace wivrn::nxastc_packet
{
inline constexpr size_t header_size = 24;
inline constexpr size_t motion_header_size = 32;
inline constexpr uint64_t independent_frame = UINT64_MAX;
enum class compression : uint8_t
{
	none = 0,
	lz4 = 1,
	zstd = 2,
	motion_zstd = 3,
	motion_raw = 4,
	compact_zstd = 5,
};

struct packet_header
{
	uint32_t width, height, raw_bytes, payload_bytes;
	compression encoding;
	size_t header_bytes = header_size;
	uint64_t reference_frame = independent_frame;
};

inline constexpr uint64_t block_bytes(uint32_t width, uint32_t height)
{
	return ((uint64_t(width) + 7) / 8) * ((uint64_t(height) + 7) / 8) * 16;
}

inline uint32_t read32(const uint8_t * p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint64_t read64(const uint8_t * p)
{
	return uint64_t(read32(p)) | (uint64_t(read32(p + 4)) << 32);
}

inline void write32(uint8_t * p, uint32_t value)
{
	for (unsigned j = 0; j < 4; ++j)
		p[j] = uint8_t(value >> (j * 8));
}

inline void write64(uint8_t * p, uint64_t value)
{
	write32(p, uint32_t(value));
	write32(p + 4, uint32_t(value >> 32));
}

inline std::array<uint8_t, header_size> make_header(uint32_t width, uint32_t height, uint32_t payload_bytes, compression encoding)
{
	const uint64_t raw = block_bytes(width, height);
	if (!width || !height || raw > std::numeric_limits<uint32_t>::max() || !payload_bytes || payload_bytes > raw ||
	    (encoding == compression::none && payload_bytes != raw) ||
	    (encoding != compression::none && encoding != compression::lz4 && encoding != compression::zstd &&
	     encoding != compression::compact_zstd) ||
	    (encoding == compression::compact_zstd && payload_bytes > compact_block_bytes(width, height)))
		throw std::invalid_argument("invalid NX ASTC packet dimensions or size");
	const uint8_t version = encoding == compression::zstd ? 2 : encoding == compression::compact_zstd ? 4 : 1;
	std::array<uint8_t, header_size> bytes{'N', 'A', 'S', 'T', version, uint8_t(encoding), 0, 0};
	const std::array values{width, height, uint32_t(raw), payload_bytes};
	for (size_t i = 0; i < values.size(); ++i)
		for (unsigned j = 0; j < 4; ++j)
			bytes[8 + i * 4 + j] = uint8_t(values[i] >> (j * 8));
	return bytes;
}

inline std::array<uint8_t, motion_header_size> make_motion_header(uint32_t width,
	                                                               uint32_t height,
	                                                               uint32_t payload_bytes,
	                                                               compression encoding,
	                                                               uint64_t reference_frame)
{
	const uint64_t raw = block_bytes(width, height);
	if (!width || !height || raw > std::numeric_limits<uint32_t>::max() || !payload_bytes ||
	    (encoding != compression::motion_zstd && encoding != compression::motion_raw) ||
	    (encoding == compression::motion_raw && (reference_frame != independent_frame || payload_bytes != raw)))
		throw std::invalid_argument("invalid NX ASTC motion packet dimensions or size");
	if (payload_bytes > raw)
		throw std::invalid_argument("NX ASTC motion payload must not exceed raw ASTC size");
	std::array<uint8_t, motion_header_size> bytes{'N', 'A', 'S', 'T', 3, uint8_t(encoding), 0, 0};
	write32(bytes.data() + 8, width);
	write32(bytes.data() + 12, height);
	write32(bytes.data() + 16, uint32_t(raw));
	write32(bytes.data() + 20, payload_bytes);
	write64(bytes.data() + 24, reference_frame);
	return bytes;
}

// Preserve v1 call sites; true has always meant LZ4.
inline std::array<uint8_t, header_size> make_header(uint32_t width, uint32_t height, uint32_t payload_bytes, bool compressed)
{
	return make_header(width, height, payload_bytes, compressed ? compression::lz4 : compression::none);
}

inline std::optional<packet_header> parse_packet(std::span<const uint8_t> bytes)
{
	if (bytes.size() < header_size || bytes[0] != 'N' || bytes[1] != 'A' || bytes[2] != 'S' || bytes[3] != 'T' || bytes[6] || bytes[7])
		return {};
	size_t hbytes = header_size;
	const auto enc = compression(bytes[5]);
	if (!((bytes[4] == 1 && (enc == compression::none || enc == compression::lz4)) ||
	      (bytes[4] == 2 && enc == compression::zstd) ||
	      (bytes[4] == 3 && (enc == compression::motion_zstd || enc == compression::motion_raw)) ||
	      (bytes[4] == 4 && enc == compression::compact_zstd)))
		return {};
	if (bytes[4] == 3)
		hbytes = motion_header_size;
	if (bytes.size() < hbytes)
		return {};
	packet_header h{read32(bytes.data() + 8), read32(bytes.data() + 12), read32(bytes.data() + 16), read32(bytes.data() + 20), enc};
	h.header_bytes = hbytes;
	if (hbytes == motion_header_size)
		h.reference_frame = read64(bytes.data() + 24);
	if (!h.width || !h.height || block_bytes(h.width, h.height) != h.raw_bytes || !h.payload_bytes ||
	    bytes.size() - hbytes != h.payload_bytes)
		return {};
	if ((h.encoding == compression::none || h.encoding == compression::lz4 || h.encoding == compression::zstd) &&
	    (h.payload_bytes > h.raw_bytes || (h.encoding == compression::none && h.payload_bytes != h.raw_bytes)))
		return {};
	if (h.encoding == compression::compact_zstd && h.payload_bytes > compact_block_bytes(h.width, h.height))
		return {};
	if (h.encoding == compression::motion_raw &&
	    (h.reference_frame != independent_frame || h.payload_bytes != h.raw_bytes))
		return {};
	if (h.encoding == compression::motion_zstd && h.payload_bytes > h.raw_bytes)
		return {};
	return h;
}
} // namespace wivrn::nxastc_packet
