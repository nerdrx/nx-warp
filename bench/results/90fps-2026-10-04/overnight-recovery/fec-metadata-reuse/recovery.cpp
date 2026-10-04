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
  const unsigned seed = argc > 1 ? unsigned(std::stoul(argv[1])) : 12345;
  constexpr unsigned iterations = 2500;
  std::mt19937 rng(seed);
  volatile uint64_t checksum = 0;
  for (uint16_t k : {uint16_t(4), uint16_t(8), uint16_t(16)}) {
    for (uint16_t stride : {uint16_t(1), uint16_t(4)}) {
      for (unsigned miss : {0u, unsigned(k/2), unsigned(k-1)}) {
        std::vector<shard> shards(k);
        parity p{};
        p.stream_item_idx = 1; p.frame_idx = 900; p.first_shard_idx = 20; p.shard_stride = stride;
        p.blob_size.resize(k);
        std::vector<std::vector<uint8_t>> blobs(k), payloads(k);
        size_t largest=0;
        for (unsigned i=0;i<k;i++) {
          auto &s=shards[i]; s.stream_item_idx=1; s.frame_idx=900; s.shard_idx=20+i*stride;
          const size_t sizes[] = {1400, 1400, 613, 97};
          const size_t n = sizes[i%4];
          payloads[i].resize(n);
          for (auto &b: payloads[i]) b=uint8_t(rng());
          s.payload=payloads[i];
          if (i==0) s.view_info = to_headset::video_stream_data_shard::view_info_t{};
          if (i==k-1) s.timing_info = to_headset::video_stream_data_shard::timing_info_t{.encode_begin=1,.encode_end=2,.send_begin=3,.send_end=4};
          fec::encode_blob(s, blobs[i]); p.blob_size[i]=blobs[i].size(); largest=std::max(largest,blobs[i].size());
        }
        std::vector<uint8_t> parity_bytes(largest,0);
        p.payload=parity_bytes;
        for (auto &b: blobs) for(size_t j=0;j<b.size();j++) parity_bytes[j]^=b[j];
        auto lookup=[&](uint16_t idx)->const shard* { for(unsigned i=0;i<k;i++) if(i!=miss && shards[i].shard_idx==idx) return &shards[i]; return nullptr; };
        auto check=fec::reconstruct(p,lookup);
        if(!check || check->payload.size()!=shards[miss].payload.size() || !std::equal(check->payload.begin(),check->payload.end(),shards[miss].payload.begin())) return 2;
        allocation_count=allocation_bytes=0; track_allocations=true;
        auto start=std::chrono::steady_clock::now();
        for(unsigned n=0;n<iterations;n++) { auto out=fec::reconstruct(p,lookup); if(!out) return 3; checksum += out->payload.size(); }
        auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        track_allocations=false;
        std::printf("%u,%u,%u,%zu,%lld,%llu,%llu\n",k,stride,miss,largest,(long long)(ns/iterations),(unsigned long long)(allocation_count/iterations),(unsigned long long)(allocation_bytes/iterations));
      }
    }
  }
  std::fprintf(stderr,"checksum=%llu\n",(unsigned long long)checksum);
}
