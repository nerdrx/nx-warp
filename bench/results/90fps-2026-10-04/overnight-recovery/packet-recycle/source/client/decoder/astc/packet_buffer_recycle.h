#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wivrn::astc_detail
{
// Caller holds the decoder mutex. Retain at most one packet-sized allocation.
inline void recycle_packet_buffer_locked(std::vector<uint8_t> & spare,
                                         std::vector<uint8_t> & packet,
                                         std::size_t max_capacity) noexcept
{
	if (packet.capacity() == 0 || packet.capacity() > max_capacity || packet.capacity() <= spare.capacity())
		return;
	spare.clear();
	packet.clear();
	spare.swap(packet);
}

// Caller holds the decoder mutex. Never replace a non-empty assembler.
inline void use_recycled_packet_buffer_locked(std::vector<uint8_t> & assembling,
                                              std::vector<uint8_t> & spare) noexcept
{
	if (assembling.capacity() == 0 && spare.capacity() != 0)
		assembling.swap(spare);
}
} // namespace wivrn::astc_detail
