#define main original_nack_test_main
#include "nack_test.cpp"
#undef main
#include <numeric>
#include <stdexcept>
void drain(shard_set &s, int64_t t) {
 for (auto &p:s.parity) s.reconstruct(p,t);
 std::erase_if(s.parity,[&](const auto&p){return s.group_complete(p);});
}
int main(){
 puts("shards,k,parity,loss,cap,rounds,asked,replied,remaining,complete");
 for(size_t n:{size_t(261),size_t(521)}) for(uint16_t k:{uint16_t(16),uint16_t(8),uint16_t(4)}) {
  auto f=make_frame(n,7,k,4);
  shard_history h;h.set_enabled(true);
  for(auto&s:f.shards){std::vector<uint8_t>b;fec::encode_blob(s,b);h.push(7,s.shard_idx,b,true);}
  for(bool parity:{false,true}) for(size_t loss:{size_t(8),size_t(32),size_t(64),size_t(96),size_t(128),size_t(192),size_t(256)}) for(size_t cap:{size_t(64),size_t(128),size_t(256)}) {
   std::vector<size_t>d(loss);std::iota(d.begin(),d.end(),size_t(4));
   auto s=receive(f,d,parity);size_t rounds=0,asked=0,replied=0;
   for(;rounds<2&&!s.complete();){
    std::vector<uint16_t>missing;s.missing_shards(missing,true);
    if(missing.empty())break;
    auto req=make_request(0,7,missing);
    for(auto b:req.bitmap)asked+=std::popcount(b);
    std::vector<shard_history::hit> hits;
    auto got=h.collect(7,req.first_shard_idx,req.bitmap,cap,hits);
    if(got!=hits.size())throw std::runtime_error("count");
    for(auto &v:hits){s.insert(fec::decode_blob(0,7,v.shard_idx,v.blob),3000+rounds);drain(s,4000+rounds);}
    replied+=got;++rounds;
   }
   size_t remaining=0;for(size_t i=0;i<n;++i){if(i>=s.data.size()||!s.data[i]){++remaining;continue;}
    const auto &a=s.data[i]->payload;const auto&b=f.shards[i].payload;
    if(a.size()!=b.size()||!std::equal(a.begin(),a.end(),b.begin()))throw std::runtime_error("payload mismatch");
   }
   if(s.complete()!=(remaining==0))throw std::runtime_error("completeness");
   printf("%zu,%u,%d,%zu,%zu,%zu,%zu,%zu,%zu,%d\n",n,k,int(parity),loss,cap,rounds,asked,replied,remaining,int(s.complete()));
  }
 }
}
