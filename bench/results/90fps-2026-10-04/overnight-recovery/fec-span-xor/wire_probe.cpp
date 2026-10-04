#include "protocol_version.h"
#include "fec.h"
#include <cstdio>
#include <vector>
using namespace wivrn;
int main() {
  std::printf("protocol=%016llx bool=%016llx view=%016llx\n", (unsigned long long)protocol_version,
    (unsigned long long)serialization_type_hash<bool>(protocol_revision),
    (unsigned long long)serialization_type_hash<to_headset::video_stream_data_shard::view_info_t>(protocol_revision));
  for (bool alpha: {false,true}) for (bool timing: {false,true}) {
    fec::data_shard s{}; s.stream_item_idx=0; s.frame_idx=77; s.shard_idx=0;
    to_headset::video_stream_data_shard::view_info_t v{}; v.display_time=1234567; v.alpha=alpha; s.view_info=v;
    if(timing) s.timing_info=to_headset::video_stream_data_shard::timing_info_t{.encode_begin=11,.encode_end=22,.send_begin=33,.send_end=44};
    std::vector<uint8_t> payload{0,1,2,127,128,254,255}; s.payload=payload;
    std::vector<uint8_t> blob; fec::encode_blob(s,blob);
    std::printf("alpha=%d timing=%d size=%zu bytes=",alpha,timing,blob.size());
    for(auto b:blob) std::printf("%02x",unsigned(b));
    std::puts("");
  }
}
