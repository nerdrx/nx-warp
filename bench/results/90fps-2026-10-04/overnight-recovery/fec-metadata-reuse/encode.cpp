#include "fec.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <new>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <span>
#include <string_view>
#include <vector>
static bool track_allocations=false;
static uint64_t allocation_count=0, allocation_bytes=0;
void *operator new(std::size_t n) { if(track_allocations){++allocation_count; allocation_bytes+=n;} if(void *p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
using namespace wivrn;
using shard = fec::data_shard;
using parity = fec::parity_shard;

int main(int argc, char **argv) {
 const unsigned seed=argc>1?unsigned(std::stoul(argv[1])):12345;
 std::mt19937 rng(seed);
 volatile uint64_t checksum=0;
 constexpr unsigned iterations=10000;
 for(size_t size:{size_t(7),size_t(31),size_t(32),size_t(1400)})for(unsigned meta=0;meta<4;meta++) {
  std::vector<uint8_t> payload(size), out;
  for(auto &b:payload)b=uint8_t(rng());
  shard s{};s.payload=payload;s.frame_idx=1;s.shard_idx=0;
  if(meta&1)s.view_info=to_headset::video_stream_data_shard::view_info_t{};
  if(meta&2)s.timing_info=to_headset::video_stream_data_shard::timing_info_t{.send_begin=1,.send_end=2};
  fec::encode_blob(s,out);
  auto decoded=fec::decode_blob(s.stream_item_idx,s.frame_idx,s.shard_idx,out);
  if(!std::equal(payload.begin(),payload.end(),decoded.payload.begin(),decoded.payload.end()))return 2;
  allocation_count=allocation_bytes=0;track_allocations=true;
  auto begin=std::chrono::steady_clock::now();
  for(unsigned i=0;i<iterations;i++) {fec::encode_blob(s,out);checksum+=out[i%out.size()];}
  auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
  track_allocations=false;
  std::printf("%zu,%u,%zu,%lld,%llu,%llu\n",size,meta,out.size(),(long long)(ns/iterations),(unsigned long long)(allocation_count/iterations),(unsigned long long)(allocation_bytes/iterations));
 }
 std::fprintf(stderr,"checksum=%llu\n",(unsigned long long)checksum);
}
