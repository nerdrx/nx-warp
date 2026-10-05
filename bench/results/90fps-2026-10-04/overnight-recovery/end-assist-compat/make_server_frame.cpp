#include "fec.h"
#include "wivrn_serialization.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <vector>

static std::vector<uint8_t> wire(const wivrn::to_headset::video_stream_data_shard & shard)
{
	wivrn::serialization_packet packet;
	packet.serialize(shard);
	std::vector<uint8_t> bytes;
	for (auto span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
		bytes.insert(bytes.end(), span.begin(), span.end());
	return bytes;
}

int main(int argc, char ** argv)
{
	if (argc != 2) return 2;
	constexpr uint64_t frame = 7331;
	constexpr uint16_t count = 5;
	std::ofstream out(argv[1], std::ios::binary);
	if (!out) return 3;
	out.write(reinterpret_cast<const char *>(&frame), sizeof(frame));
	out.write(reinterpret_cast<const char *>(&count), sizeof(count));
	std::vector<uint8_t> payload(64), blob;
	for (uint16_t index = 0; index < count; ++index)
	{
		for (size_t i = 0; i < payload.size(); ++i)
			payload[i] = uint8_t((frame * 31 + uint64_t(index) * 17 + i * 13) & 0xff);
		wivrn::to_headset::video_stream_data_shard shard{};
		shard.stream_item_idx = 0;
		shard.frame_idx = frame;
		shard.shard_idx = index;
		if (index == 0)
		{
			shard.view_info.emplace();
			shard.view_info->foveation = {
			        wivrn::to_headset::foveation_parameter{{1, 4, 5, 3, 1}, {2, 3, 4, 3, 2}},
			        wivrn::to_headset::foveation_parameter{{1, 4, 5, 3, 1}, {2, 3, 4, 3, 2}},
			};
		}
		if (index + 1 == count)
			shard.timing_info = wivrn::to_headset::video_stream_data_shard::timing_info_t{101, 102, 103, 104};
		shard.payload = payload;
		wivrn::fec::encode_blob(shard, blob);
		auto rebuilt = wivrn::fec::decode_blob(0, frame, index, blob);
		auto packet = wire(shard);
		if (wire(rebuilt) != packet) return 5;
		const uint32_t size = uint32_t(blob.size());
		const uint32_t packet_size = uint32_t(packet.size());
		out.write(reinterpret_cast<const char *>(&index), sizeof(index));
		out.write(reinterpret_cast<const char *>(&size), sizeof(size));
		out.write(reinterpret_cast<const char *>(blob.data()), std::streamsize(blob.size()));
		out.write(reinterpret_cast<const char *>(&packet_size), sizeof(packet_size));
		out.write(reinterpret_cast<const char *>(packet.data()), std::streamsize(packet.size()));
	}
	return out ? 0 : 4;
}
