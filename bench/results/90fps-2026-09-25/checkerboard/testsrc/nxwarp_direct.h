// NX direct blocks v1/v2: bounded, little-endian, independent-frame format.
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
namespace wivrn::nxwarp_direct
{
constexpr uint32_t stream_magic = 0x4244584e, frame_magic = 0x4644584e, version = 1, native_version = 2;
constexpr uint32_t native_rgb_words = 1025;
constexpr uint32_t checker_flag = 0x100, phase_flag = 0x200;
constexpr size_t stream_header_bytes = 32, frame_header_bytes = 16;
struct layout
{
	uint32_t width = 0, height = 0, eyes = 0; // width per eye
	bool native_center = false;
	uint32_t native_side = 256;
	bool packed_native = true;
	bool zstd = false;
	bool predictor = false;
	bool checkerboard = false;
	bool valid() const
	{
		return width && height && width <= 4096 && height <= 4096 &&
		       (eyes == 1 || eyes == 2) && width % 32 == 0 && height % 32 == 0;
	}
	uint32_t tile_count() const
	{
		return (width / 32) * (height / 32) * eyes;
	}
	uint32_t max_block_words() const
	{
		return tile_count() * 80 + (native_center ? (native_side / 32) * (native_side / 32) * eyes * native_rgb_words : 0);
	}
	uint32_t max_frame_bytes() const
	{
		return 16 + 4 * (tile_count() + max_block_words());
	}
};
inline uint32_t read32(std::span<const uint8_t> b, size_t p)
{
	return uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8) | (uint32_t(b[p + 2]) << 16) | (uint32_t(b[p + 3]) << 24);
}
inline void append32(std::vector<uint8_t> & b, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		b.push_back(uint8_t(v >> (8 * i)));
}
inline bool is_stream(std::span<const uint8_t> b)
{
	return b.size() >= 4 && read32(b, 0) == stream_magic;
}
// Versions 1/2: raw; 3/4: optional LZ4; 5/6: safety prefix and optional LZ4.
// Versions 7/8 are the native-RGB variants of 5/6. Even versions use trusted-LAN CRC.
// Versions 9/10 expand the native container to 256x256.
// Versions 11/12 additionally permit packed RGB565 native pixels (descriptor bit 28).
// Versions 13/14: RGB888 + optional Zstd; 15/16: RGB565 + optional Zstd.
// Versions 17/18: RGB888 + optional byte predictor; 19/20: RGB565 + predictor.
// Native RGB tiles use NXDF v2; legacy payloads remain v1.
// Older clients reject unsupported stream versions.
// Add 32 to a base stream version to permit alternating checkerboard samples.
inline std::vector<uint8_t> stream_header(layout l, bool trusted_lan = false, bool lz4 = false, bool safety = false)
{
	if (!l.valid() || (l.zstd && !l.native_center) || (l.predictor && !l.zstd) || (l.native_center && (l.packed_native || l.zstd) && l.native_side != 256))
		return {};
	std::vector<uint8_t> b;
	b.reserve(32);
	if (l.native_center && (!safety || !lz4 || l.eyes != 2 || l.width < l.native_side || l.height < l.native_side || (l.native_side != 128 && l.native_side != 256)))
		return {};
	// Native streams always use the safety+LZ4 envelope; 7/8 retain the
	// existing odd/even CRC/trusted-LAN distinction.
	const uint32_t base = l.native_center ? (l.zstd ? (l.predictor ? (l.packed_native ? 19u : 17u) : (l.packed_native ? 15u : 13u)) : l.packed_native ? 11u
	                                                                          : l.native_side == 128  ? 7u
	                                                                                                  : 9u)
	                                      : (safety ? 5u : lz4 ? 3u
	                                                           : 1u);
	for (uint32_t v: {stream_magic, base + (trusted_lan ? 1u : 0u) + (l.checkerboard ? 32u : 0u), l.width, l.height, l.eyes, l.tile_count(), l.max_block_words(), l.max_frame_bytes()})
		append32(b, v);
	return b;
}
inline std::optional<layout> parse_stream(std::span<const uint8_t> b)
{
	if (b.size() != 32 || !is_stream(b) || read32(b, 4) > 52)
		return {};
	const uint32_t stream_version = read32(b, 4) & 31u;
	if (stream_version < 1 || stream_version > 20)
		return {};
	const bool native = stream_version >= 7;
	const bool safety = stream_version >= 5;
	if (native && !safety)
		return {};
	layout l{read32(b, 8), read32(b, 12), read32(b, 16), native, stream_version >= 9 ? 256u : 128u,
	          (stream_version >= 11 && stream_version <= 12) || (stream_version >= 15 && stream_version <= 16) || stream_version >= 19,
	          stream_version >= 13, stream_version >= 17, read32(b, 4) >= 32};
	if (!l.valid() || (native && (l.eyes != 2 || l.width < l.native_side || l.height < l.native_side)) || read32(b, 20) != l.tile_count() || read32(b, 24) != l.max_block_words() || read32(b, 28) != l.max_frame_bytes())
		return {};
	return l;
}
struct frame_view
{
	std::span<const uint8_t> descriptors, blocks;
};
inline bool checker_frame(std::span<const uint8_t> b)
{
	return b.size() >= frame_header_bytes && (read32(b, 4) & checker_flag);
}
inline uint32_t checker_phase(std::span<const uint8_t> b)
{
	return b.size() >= frame_header_bytes && (read32(b, 4) & phase_flag) ? 1u : 0u;
}
inline std::optional<frame_view> parse_frame(layout l, std::span<const uint8_t> b)
{
	if (!l.valid() || b.size() < 16 || b.size() > l.max_frame_bytes() || read32(b, 0) != frame_magic)
		return {};
	const uint32_t flags = read32(b, 4), frame_version = flags & 255u;
	const bool checker = flags & checker_flag;
	if ((flags & ~(255u | checker_flag | phase_flag)) || (checker && !l.checkerboard) || (!checker && (flags & phase_flag)))
		return {};
	if (frame_version == native_version ? !l.native_center : frame_version != version)
		return {};
	uint32_t n = read32(b, 8), words = read32(b, 12);
	if (n != l.tile_count() || words > l.max_block_words() || (!checker && words % 5) || b.size() != 16ull + 4ull * (n + words))
		return {};
	frame_view v{b.subspan(16, n * 4), b.subspan(16 + n * 4, words * 4)};
	for (uint32_t i = 0; i < n; i++)
	{
		uint32_t d = read32(v.descriptors, i * 4), mode = d >> 30;
		if (d & 0x20000000u)
		{
			if (!l.native_center || frame_version != native_version || mode != 0)
				return {};
			const bool packed = d & 0x10000000u;
			if (packed && !l.packed_native)
				return {};
			const uint32_t offset = d & 0x0fffffffu;
			const uint32_t count = checker ? (packed ? 256u : 512u) : (packed ? 515u : native_rgb_words);
			if ((!checker && offset % 5) || uint64_t(offset) + count > words)
				return {};
			continue;
		}
		if (mode == 3)
		{
			if (d & 0x3f000000u)
				return {};
			continue;
		} // inline 0xRRGGBB
		uint32_t offset = d & 0x3fffffffu, count = (checker ? 48u : 80u) >> (mode * 2);
		if ((!checker && offset % 5) || uint64_t(offset) + count > words)
			return {};
	}
	return v;
}
inline std::vector<uint8_t> frame_header(uint32_t descriptors, uint32_t words, uint32_t frame_version = version)
{
	std::vector<uint8_t> b;
	b.reserve(16);
	for (uint32_t v: {frame_magic, frame_version, descriptors, words})
		append32(b, v);
	return b;
}
} // namespace wivrn::nxwarp_direct
