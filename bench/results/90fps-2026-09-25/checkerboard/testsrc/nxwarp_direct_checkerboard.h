// Remove alternate encoded samples. Shared palette endpoints remain independent.
#pragma once
#include "nxwarp_direct.h"

namespace wivrn::nxwarp_direct
{
inline std::span<const uint8_t> checkerboard_frame(layout l, std::span<const uint8_t> raw,
                                                 std::vector<uint8_t> & out, uint32_t phase)
{
	const auto parsed = parse_frame(l, raw);
	if (!l.checkerboard || !parsed || checker_frame(raw) || phase > 1)
		return {};
	out.clear();
	out.reserve(raw.size());
	for (uint32_t value: {frame_magic, read32(raw, 4) | checker_flag | (phase ? phase_flag : 0u), l.tile_count(), 0u})
		append32(out, value);
	out.resize(frame_header_bytes + l.tile_count() * 4);
	uint32_t words = 0;
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile)
	{
		const uint32_t old = read32(parsed->descriptors, tile * 4), mode = old >> 30;
		uint32_t descriptor = old;
		if (old & 0x20000000u)
		{
			const bool packed = old & 0x10000000u;
			const uint32_t offset = old & 0x0fffffffu;
			descriptor = (old & 0xf0000000u) | words;
			uint32_t pair = 0, count = 0;
			for (uint32_t y = 0; y < 32; ++y)
				for (uint32_t x = (y & 1u) ^ phase; x < 32; x += 2)
				{
					const uint32_t index = y * 32 + x;
					if (!packed)
						append32(out, read32(parsed->blocks, 4 * (offset + index)));
					else
					{
						const uint32_t pixel = (read32(parsed->blocks, 4 * (offset + index / 2)) >> (16 * (index % 2))) & 65535u;
						if (count % 2 == 0) pair = pixel;
						else append32(out, pair | (pixel << 16));
					}
					++count;
				}
			words += packed ? 256 : 512;
		}
		else if (mode != 3)
		{
			const uint32_t offset = old & 0x3fffffffu, blocks = 16u >> (2 * mode);
			descriptor = (mode << 30) | words;
			for (uint32_t block = 0; block < blocks; ++block)
			{
				append32(out, read32(parsed->blocks, 4 * (offset + 5 * block)));
				uint32_t selectors[2]{};
				for (uint32_t y = 0; y < 8; ++y)
					for (uint32_t x = (y & 1u) ^ phase; x < 8; x += 2)
					{
						const uint32_t index = y * 8 + x, compact = y * 4 + x / 2;
						const uint32_t selector = (read32(parsed->blocks, 4 * (offset + 5 * block + 1 + index / 16)) >> (2 * (index % 16))) & 3u;
						selectors[compact / 16] |= selector << (2 * (compact % 16));
					}
				append32(out, selectors[0]);
				append32(out, selectors[1]);
			}
			words += 3 * blocks;
		}
		for (unsigned byte = 0; byte < 4; ++byte)
			out[frame_header_bytes + tile * 4 + byte] = uint8_t(descriptor >> (8 * byte));
	}
	for (unsigned byte = 0; byte < 4; ++byte)
		out[12 + byte] = uint8_t(words >> (8 * byte));
	return out;
}
} // namespace wivrn::nxwarp_direct
