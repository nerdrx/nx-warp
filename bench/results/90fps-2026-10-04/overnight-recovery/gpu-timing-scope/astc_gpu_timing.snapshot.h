#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace wivrn::astc_gpu_timing
{
// Timestamps from one queue share a counter. Mask subtraction handles one wrap;
// Vulkan does not provide enough information to distinguish multiple wraps.
inline std::optional<double> elapsed_ms(uint64_t begin, uint64_t end, uint32_t valid_bits, double period_ns)
{
	if (valid_bits == 0 || valid_bits > 64 || !(period_ns > 0) || !std::isfinite(period_ns))
		return {};

	const uint64_t mask = valid_bits == 64
	                              ? std::numeric_limits<uint64_t>::max()
	                              : (uint64_t{1} << valid_bits) - 1;
	const uint64_t ticks = (end - begin) & mask;
	const double ms = double(ticks) * period_ns / 1'000'000.0;
	return std::isfinite(ms) ? std::optional<double>{ms} : std::nullopt;
}
} // namespace wivrn::astc_gpu_timing
