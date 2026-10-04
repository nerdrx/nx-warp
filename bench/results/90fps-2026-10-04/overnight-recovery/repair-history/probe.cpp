#include "fec.h"
#include "shard_history.h"
#include <cstdio>
#include <stdexcept>
using namespace wivrn;
int main(){
 puts("capacity,aggregate_payload_mbps,source_fps,fec,frame_payload_bytes,frame_blob_bytes,shards,age_frames,retained_shards,retained_payload_bytes");
 for(int rate:{250,500,700,1000})for(bool enabled:{false,true}){
  shard_history h;h.set_enabled(true);size_t bytes=size_t(rate)*1000000/8/90/2;
  std::vector<uint8_t> payload(bytes,0x5a),blob;size_t count=0,stored=0;
  for(uint64_t frame=0;frame<12;++frame){
   size_t off=0;uint16_t index=0;stored=0;
   while(off<bytes){to_headset::video_stream_data_shard s{};s.frame_idx=frame;s.shard_idx=index;if(index==0)s.view_info.emplace();
    size_t n=std::min(bytes-off,fec::shard_payload_budget(enabled)-serialized_size(s.view_info));
    s.payload=std::span(payload).subspan(off,n);if(off+n==bytes)s.timing_info.emplace();
    fec::encode_blob(s,blob);stored+=blob.size();h.push(frame,index,blob,true);off+=n;++index;
   }count=index;
  }
  for(int age=0;age<=3;++age){size_t found=0,retained=0;
   for(size_t first=0;first<count;first+=64){std::array<uint8_t,8>bitmap;bitmap.fill(0xff);std::vector<shard_history::hit> hits;
    h.collect(11-age,uint16_t(first),bitmap,64,hits);
    for(auto &hit:hits){auto s=fec::decode_blob(0,11-age,hit.shard_idx,hit.blob);for(auto v:s.payload)if(v!=0x5a)throw std::runtime_error("overwritten bytes");retained+=s.payload.size();++found;}
   }
   printf("%zu,%d,90,%d,%zu,%zu,%zu,%d,%zu,%zu\n",shard_history::capacity,rate,enabled,bytes,stored,count,age,found,retained);
  }
 }
}