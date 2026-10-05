#include "shard_set.h"             // c514841f client/decoder
#include "shard_history.h"         // current server helper
#include "wivrn_serialization.h"   // c514841f common

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

using namespace wivrn;
using data_shard = to_headset::video_stream_data_shard;

static std::vector<uint8_t> flatten(serialization_packet & packet)
{
	std::vector<uint8_t> bytes;
	for (auto span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
		bytes.insert(bytes.end(), span.begin(), span.end());
	return bytes;
}

static data_shard receive_current_server_packet(std::span<const uint8_t> bytes)
{
	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[bytes.size() + 1]);
	std::memcpy(memory.get(), bytes.data(), bytes.size());
	deserialization_packet decoded{memory, std::span<uint8_t>(memory.get(), bytes.size())};
	return decoded.deserialize<data_shard>();
}

int main(int argc, char ** argv)
{
	if (argc != 2) return 2;
	std::ifstream in(argv[1], std::ios::binary);
	uint64_t frame = 0;
	uint16_t count = 0;
	in.read(reinterpret_cast<char *>(&frame), sizeof(frame));
	in.read(reinterpret_cast<char *>(&count), sizeof(count));
	if (!in || frame != 7331 || count != 5) return 3;

	shard_history current_server;
	current_server.set_enabled(true);
	shard_set old_client(0);
	std::vector<std::vector<uint8_t>> blobs(count);
	std::vector<std::vector<uint8_t>> packets(count);
	for (uint16_t i = 0; i < count; ++i)
	{
		uint16_t index = 0;
		uint32_t size = 0;
		in.read(reinterpret_cast<char *>(&index), sizeof(index));
		in.read(reinterpret_cast<char *>(&size), sizeof(size));
		blobs[i].resize(size);
		in.read(reinterpret_cast<char *>(blobs[i].data()), size);
		uint32_t packet_size = 0;
		in.read(reinterpret_cast<char *>(&packet_size), sizeof(packet_size));
		packets[i].resize(packet_size);
		in.read(reinterpret_cast<char *>(packets[i].data()), packet_size);
		if (!in || index != i) return 4;
		// Compare both current representations through the archived parsers.
		auto recovered = fec::decode_blob(0, frame, index, blobs[i]);
		auto received = receive_current_server_packet(packets[i]);
		if (recovered.stream_item_idx != received.stream_item_idx ||
		    recovered.frame_idx != received.frame_idx || recovered.shard_idx != received.shard_idx ||
		    recovered.payload.size() != 64 || received.payload.size() != 64 ||
		    !std::equal(recovered.payload.begin(), recovered.payload.end(), received.payload.begin()) ||
		    bool(recovered.view_info) != bool(received.view_info) ||
		    bool(recovered.timing_info) != bool(received.timing_info)) return 16;
		current_server.push(frame, index, blobs[i], true);
	}
	current_server.end_frame(frame, count);

	// Recreate the archived receiver's partial frame: shards 0, 2, 3 arrived;
	// shard 1 is an ordinary known-prefix hole and shard 4's end marker is lost.
	for (uint16_t index: {uint16_t(0), uint16_t(2), uint16_t(3)})
		old_client.insert(receive_current_server_packet(packets[index]), 1);
	std::vector<uint16_t> missing;
	old_client.missing_shards(missing, false);
	if (old_client.complete() || missing != std::vector<uint16_t>{1}) return 5;

	// Build and round-trip the actual archived NACK representation.
	from_headset::nack nack{.stream_index = 0, .frame_idx = frame,
	                       .first_shard_idx = 1, .bitmap = {1}};
	serialization_packet request_wire;
	request_wire.serialize(nack);
	auto request_bytes = flatten(request_wire);
	auto request_memory = std::shared_ptr<uint8_t[]>(new uint8_t[request_bytes.size() + 1]);
	std::memcpy(request_memory.get(), request_bytes.data(), request_bytes.size());
	deserialization_packet request_in{request_memory,
	                                  std::span<uint8_t>(request_memory.get(), request_bytes.size())};
	auto parsed = request_in.deserialize<from_headset::nack>();
	if (parsed.stream_index != 0 || parsed.frame_idx != frame || parsed.first_shard_idx != 1 ||
	    parsed.bitmap != std::vector<uint8_t>{1}) return 6;

	// Current server ordinary bitmap response, then the bounded real-terminal helper.
	std::vector<shard_history::hit> hits;
	if (current_server.collect(parsed.frame_idx, parsed.first_shard_idx, parsed.bitmap, 64, hits) != 1 ||
	    hits[0].shard_idx != 1) return 7;
	std::vector<shard_history::hit> ordinary = hits;
	auto end = current_server.collect_frame_end_candidate(parsed.frame_idx, ordinary, 64 - hits.size());
	if (!end || end->shard_idx != 4) return 8;

	for (auto & hit: hits)
		old_client.insert(receive_current_server_packet(packets[hit.shard_idx]), 2);
	old_client.missing_shards(missing, false);
	if (old_client.complete() || !missing.empty()) return 9; // no guessed tail, still waiting for end marker

	auto terminal = fec::decode_blob(0, frame, end->shard_idx, end->blob);
	if (terminal.stream_item_idx != 0 || !terminal.timing_info ||
	    terminal.timing_info->encode_begin != 101 || terminal.timing_info->encode_end != 102 ||
	    terminal.timing_info->send_begin != 103 || terminal.timing_info->send_end != 104) return 10;
	old_client.insert(receive_current_server_packet(packets[end->shard_idx]), 3);
	old_client.missing_shards(missing, false);
	if (!old_client.complete() || !missing.empty()) return 11;

	for (uint16_t index = 0; index < count; ++index)
	{
		const auto & shard = old_client.data[index];
		if (!shard || shard->stream_item_idx != 0 || shard->frame_idx != frame ||
		    shard->shard_idx != index || shard->payload.size() != 64) return 12;
		if (index == 0 && (!shard->view_info ||
		                   shard->view_info->foveation[0].x != std::vector<uint16_t>{1, 4, 5, 3, 1} ||
		                   shard->view_info->foveation[0].y != std::vector<uint16_t>{2, 3, 4, 3, 2})) return 14;
		if (index + 1 == count && (!shard->timing_info ||
		                          shard->timing_info->encode_begin != 101 ||
		                          shard->timing_info->encode_end != 102 ||
		                          shard->timing_info->send_begin != 103 ||
		                          shard->timing_info->send_end != 104)) return 15;
		for (size_t i = 0; i < shard->payload.size(); ++i)
			if (shard->payload[i] != uint8_t((frame * 31 + uint64_t(index) * 17 + i * 13) & 0xff)) return 13;
	}
	std::cout << "old_client_frame=" << frame << " nack=1 ordinary_replies=1 assisted_terminal=1 "
	          << "complete=1 payload_checks=" << (count * 64) << " failures=0\n";
	return 0;
}
