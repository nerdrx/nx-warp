// Optional lossless NXDL envelope around independent NXDF units.
#pragma once
#include "nxwarp_direct.h"
#include <algorithm>
#include <cstring>
#include <lz4.h>
#include <lz4hc.h>

namespace wivrn::nxwarp_direct
{
constexpr uint32_t lz4_magic = 0x4c44584e, lz4_chunk_bytes = 65536;
inline bool is_lz4(std::span<const uint8_t> b)
{
	return b.size() >= 4 && read32(b, 0) == lz4_magic;
}
// Returns the original span unless complete envelope savings reach 5%.
// Caller owns both raw input and reusable output until transport copies them.
inline std::span<const uint8_t> compress_lz4_impl(std::span<const uint8_t> raw, std::vector<uint8_t> & out,
                                                  int hc_level)
{
	out.clear();
	if (raw.size() < 16 || raw.size() > layout{4096, 4096, 2, true}.max_frame_bytes())
		return raw;
	const uint32_t count = (raw.size() + lz4_chunk_bytes - 1) / lz4_chunk_bytes;
	out.reserve(16 + raw.size() + count * 12);
	for (uint32_t v: {lz4_magic, 1u, uint32_t(raw.size()), count})
		append32(out, v);
	for (size_t pos = 0; pos < raw.size(); pos += lz4_chunk_bytes)
	{
		const uint32_t n = std::min<size_t>(lz4_chunk_bytes, raw.size() - pos);
		const size_t header = out.size();
		out.resize(header + 12 + LZ4_compressBound(n));
		const int packed = hc_level < 0
			? LZ4_compress_default(reinterpret_cast<const char *>(raw.data() + pos),
			                       reinterpret_cast<char *>(out.data() + header + 12), n,
			                       LZ4_compressBound(n))
			: LZ4_compress_HC(reinterpret_cast<const char *>(raw.data() + pos),
			                  reinterpret_cast<char *>(out.data() + header + 12), n,
			                  LZ4_compressBound(n), hc_level);
		const bool compressed = packed > 0 && uint32_t(packed) < n;
		const uint32_t stored = compressed ? uint32_t(packed) : n;
		if (!compressed)
			std::memcpy(out.data() + header + 12, raw.data() + pos, n);
		for (unsigned i = 0; i < 3; ++i)
		{
			const uint32_t v = i == 0 ? n : i == 1 ? stored
			                                       : uint32_t(compressed);
			for (unsigned k = 0; k < 4; ++k)
				out[header + i * 4 + k] = uint8_t(v >> (8 * k));
		}
		out.resize(header + 12 + stored);
	}
	return out.size() * 100 <= raw.size() * 95 ? std::span<const uint8_t>(out) : raw;
}
inline std::span<const uint8_t> compress_lz4(std::span<const uint8_t> raw, std::vector<uint8_t> & out)
{
	return compress_lz4_impl(raw, out, -1);
}
// Opt-in high-compression variant. The wire format and fallback threshold match compress_lz4.
inline std::span<const uint8_t> compress_lz4_hc(std::span<const uint8_t> raw, std::vector<uint8_t> & out,
                                                int level = LZ4HC_CLEVEL_MIN)
{
	return compress_lz4_impl(raw, out, std::clamp(level, LZ4HC_CLEVEL_MIN, LZ4HC_CLEVEL_MAX));
}
inline bool decompress_lz4(layout l, std::span<const uint8_t> b, std::vector<uint8_t> & out)
{
	if (!l.valid() || b.size() < 16 || !is_lz4(b) || read32(b, 4) != 1)
		return false;
	const uint32_t size = read32(b, 8), count = read32(b, 12);
	if (size < 16 || size > l.max_frame_bytes() || b.size() >= size ||
	    count != (size + lz4_chunk_bytes - 1) / lz4_chunk_bytes)
		return false;
	// Validate the entire table before allocation or decompression.
	size_t offset = 16, produced = 0;
	for (uint32_t i = 0; i < count; ++i)
	{
		if (b.size() - offset < 12)
			return false;
		const uint32_t n = read32(b, offset), stored = read32(b, offset + 4), flags = read32(b, offset + 8);
		offset += 12;
		if (n != std::min<size_t>(lz4_chunk_bytes, size - produced) || !stored ||
		    stored > b.size() - offset || flags > 1 ||
		    (flags == 0 ? stored != n : stored >= n))
			return false;
		offset += stored;
		produced += n;
	}
	if (offset != b.size() || produced != size)
		return false;
	out.resize(size);
	offset = 16;
	produced = 0;
	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t n = read32(b, offset), stored = read32(b, offset + 4), flags = read32(b, offset + 8);
		offset += 12;
		if (flags)
		{
			if (LZ4_decompress_safe(reinterpret_cast<const char *>(b.data() + offset),
			                        reinterpret_cast<char *>(out.data() + produced),
			                        stored,
			                        n) != int(n))
				return false;
		}
		else
			std::memcpy(out.data() + produced, b.data() + offset, n);
		offset += stored;
		produced += n;
	}
	return true;
}
} // namespace wivrn::nxwarp_direct
