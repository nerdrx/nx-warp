#pragma once

#include "nxastc_packet.h"
#include "nxastc_motion.h"

#include <climits>
#include <cstring>
#include <lz4.h>
#include <span>
#include <zstd.h>

namespace wivrn::nxastc_packet
{
enum class decode_status
{
	ok,
	length_mismatch,
	lz4_mismatch,
	zstd_bad_frame,
	zstd_trailing_data,
	zstd_unknown_content_size,
	zstd_wrong_content_size,
	zstd_decode_error,
	motion_bad_selector,
	motion_reference_mismatch,
};

inline const char * decode_status_message(decode_status status)
{
	switch (status)
	{
	case decode_status::ok: return "ok";
	case decode_status::length_mismatch: return "payload/output length mismatch";
	case decode_status::lz4_mismatch: return "LZ4 payload did not decode to exact block length";
	case decode_status::zstd_bad_frame: return "invalid Zstd frame";
	case decode_status::zstd_trailing_data: return "Zstd payload has trailing data";
	case decode_status::zstd_unknown_content_size: return "Zstd frame has no declared content size";
	case decode_status::zstd_wrong_content_size: return "Zstd frame content size mismatch";
	case decode_status::zstd_decode_error: return "Zstd payload did not decode to exact block length";
	case decode_status::motion_bad_selector: return "motion selector is outside 0..8";
	case decode_status::motion_reference_mismatch: return "motion reference or output length mismatch";
	}
	return "unknown ASTC payload error";
}

// Decode into caller-owned storage; no allocation and no state retained.
inline decode_status decode_payload(const packet_header & header,
                                   std::span<const uint8_t> payload,
                                   std::span<uint8_t> output)
{
	if (payload.size() != header.payload_bytes || output.size() != header.raw_bytes)
		return decode_status::length_mismatch;
	if (header.encoding == compression::none)
	{
		if (payload.size() != output.size())
			return decode_status::length_mismatch;
		std::memcpy(output.data(), payload.data(), payload.size());
		return decode_status::ok;
	}
	if (header.encoding == compression::lz4)
	{
		if (payload.size() > INT_MAX || output.size() > INT_MAX)
			return decode_status::length_mismatch;
		const int decoded = LZ4_decompress_safe(reinterpret_cast<const char *>(payload.data()),
		                                        reinterpret_cast<char *>(output.data()),
		                                        int(payload.size()),
		                                        int(output.size()));
		return decoded == int(output.size()) ? decode_status::ok : decode_status::lz4_mismatch;
	}
	const bool compact = header.encoding == compression::compact_zstd;
	if (header.encoding != compression::zstd && !compact)
		return decode_status::length_mismatch;
	size_t zstd_output_bytes = output.size();
	if (compact)
	{
		if (!header.width || !header.height || block_bytes(header.width, header.height) != output.size() ||
		    output.size() != header.raw_bytes || !compact_bytes_for_raw(output.size(), zstd_output_bytes))
			return decode_status::length_mismatch;
	}

	const size_t frame_size = ZSTD_findFrameCompressedSize(payload.data(), payload.size());
	if (ZSTD_isError(frame_size))
		return decode_status::zstd_bad_frame;
	if (frame_size != payload.size())
		return decode_status::zstd_trailing_data;
	const unsigned long long content_size = ZSTD_getFrameContentSize(payload.data(), payload.size());
	if (content_size == ZSTD_CONTENTSIZE_ERROR)
		return decode_status::zstd_bad_frame;
	if (content_size == ZSTD_CONTENTSIZE_UNKNOWN)
		return decode_status::zstd_unknown_content_size;
	if (content_size != zstd_output_bytes)
		return decode_status::zstd_wrong_content_size;
	const size_t decoded = ZSTD_decompress(output.data(), zstd_output_bytes, payload.data(), payload.size());
	if (ZSTD_isError(decoded) || decoded != zstd_output_bytes)
		return decode_status::zstd_decode_error;
	if (compact && !expand_compact_blocks(output, zstd_output_bytes))
		return decode_status::length_mismatch;
	return decode_status::ok;
}

inline decode_status decode_motion_payload(const packet_header & header,
                                           std::span<const uint8_t> payload,
                                           std::span<const uint8_t> reference,
                                           std::span<uint8_t> output,
                                           std::span<uint8_t> scratch)
{
	if (header.header_bytes != motion_header_size ||
	    (header.encoding != compression::motion_raw && header.encoding != compression::motion_zstd) ||
	    !header.width || !header.height || block_bytes(header.width, header.height) != header.raw_bytes ||
	    payload.size() != header.payload_bytes || output.size() != header.raw_bytes)
		return decode_status::length_mismatch;
	const bool anchor = header.reference_frame == independent_frame;
	if (header.encoding == compression::motion_raw)
	{
		if (!anchor || !reference.empty() || payload.size() != output.size())
			return decode_status::motion_reference_mismatch;
		std::memcpy(output.data(), payload.data(), output.size());
		return decode_status::ok;
	}
	if (anchor)
	{
		if (!reference.empty())
			return decode_status::motion_reference_mismatch;
		auto zstd_header = header;
		zstd_header.encoding = compression::zstd;
		return decode_payload(zstd_header, payload, output);
	}
	const uint32_t block_width = uint32_t((uint64_t(header.width) + 7) / 8);
	const uint32_t block_height = uint32_t((uint64_t(header.height) + 7) / 8);
	size_t expected_raw = 0, expected_packed = 0;
	if (!detail::motion_sizes(block_width, block_height, expected_raw, expected_packed) ||
	    expected_raw != output.size() || reference.size() != expected_raw || scratch.size() != expected_packed ||
	    expected_packed > std::numeric_limits<uint32_t>::max())
		return decode_status::motion_reference_mismatch;
	auto zstd_header = header;
	zstd_header.encoding = compression::zstd;
	zstd_header.raw_bytes = uint32_t(expected_packed);
	const auto status = decode_payload(zstd_header, payload, scratch);
	if (status != decode_status::ok)
		return status;
	if (reconstruct_motion_blocks(block_width, block_height, reference, scratch, output))
		return decode_status::ok;
	return decode_status::motion_bad_selector;
}
} // namespace wivrn::nxastc_packet
