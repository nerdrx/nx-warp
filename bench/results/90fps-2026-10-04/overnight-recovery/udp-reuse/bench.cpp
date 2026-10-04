#include "wivrn_sockets.h"
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <sys/socket.h>
#include <unistd.h>

namespace {
std::atomic<bool> count_allocations{false};
std::atomic<unsigned long long> counted{0};
std::atomic<size_t> observed_size{0};
void note(size_t n) noexcept {
  if (count_allocations.load(std::memory_order_relaxed) && n >= 40960 && n <= 41216) {
    counted.fetch_add(1, std::memory_order_relaxed);
    observed_size.store(n, std::memory_order_relaxed);
  }
}
constexpr int batches = 400;
constexpr int retained_batches = 16;
constexpr int packets_per_batch = 20;
constexpr size_t payload_size = 1400;
constexpr int blocks = batches / retained_batches;
}
void *operator new(std::size_t n) { note(n); if (auto p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void *operator new[](std::size_t n) { note(n); if (auto p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

int main() {
  int rx = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), tx = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (rx < 0 || tx < 0) { std::perror("socket"); return 2; }
  int rcvbuf = 4 * 1024 * 1024; setsockopt(rx, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if (bind(rx, (sockaddr*)&addr, sizeof(addr)) != 0) { std::perror("bind"); return 2; }
  socklen_t alen=sizeof(addr); if (getsockname(rx,(sockaddr*)&addr,&alen)!=0) return 2;
  wivrn::UDP receiver(rx);
  std::array<std::array<uint8_t,payload_size>,packets_per_batch> payloads{};
  std::array<wivrn::deserialization_packet,retained_batches*packets_per_batch> held{};
  unsigned long long elapsed_ns=0;
  unsigned long long total_allocs=0;
  std::array<unsigned long long,batches> batch_ns{};
  for (int block=0; block<blocks; ++block) {
    counted.store(0, std::memory_order_relaxed);
    for (int b=0; b<retained_batches; ++b) {
      const int batch=block*retained_batches+b;
      for (int p=0; p<packets_per_batch; ++p) {
        auto & bytes=payloads[p]; bytes.fill(uint8_t((batch*17+p*31)&255));
        bytes[0]=uint8_t(batch&255); bytes[1]=uint8_t(p);
        auto n=sendto(tx,bytes.data(),bytes.size(),0,(sockaddr*)&addr,sizeof(addr));
        if (n != ssize_t(bytes.size())) { std::perror("sendto"); return 3; }
      }
      auto begin=std::chrono::steady_clock::now();
      count_allocations.store(true,std::memory_order_relaxed);
      auto packet=receiver.receive_raw();
      held[b*packets_per_batch]=std::move(packet);
      for (int p=1;p<packets_per_batch;++p) held[b*packets_per_batch+p]=receiver.receive_pending();
      count_allocations.store(false,std::memory_order_relaxed);
      auto end=std::chrono::steady_clock::now();
      auto duration=std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count();
      elapsed_ns += duration;
      batch_ns[batch]=static_cast<unsigned long long>(duration);
    }
    for (int b=0;b<retained_batches;++b) for (int p=0;p<packets_per_batch;++p) {
      const auto & bytes=held[b*packets_per_batch+p].initial_buffer;
      const int batch=block*retained_batches+b;
      if (bytes.size()!=payload_size || bytes[0]!=uint8_t(batch&255) || bytes[1]!=uint8_t(p) || bytes[2]!=uint8_t((batch*17+p*31)&255)) {
        std::fprintf(stderr,"payload/lifetime check failed block=%d batch=%d packet=%d size=%zu got=%u,%u,%u expected=%u,%u,%u\n",block,batch,p,bytes.size(), bytes.empty()?0:bytes[0], bytes.size()>1?bytes[1]:0, bytes.size()>2?bytes[2]:0, batch&255,p,(batch*17+p*31)&255); return 4;
      }
    }
    total_allocs += counted.load();
    std::printf("block=%d batches=%d matching_allocs=%llu observed_size=%zu\n",block+1,retained_batches,counted.load(),observed_size.load());
    for(auto & p:held) p=wivrn::deserialization_packet{};
  }
  auto sorted_ns=batch_ns; std::sort(sorted_ns.begin(),sorted_ns.end());
  std::printf("RESULT blocks=%d batches=%d datagrams=%d payload_bytes=%zu receive_ns=%llu per_batch_p50_ns=%llu per_batch_p95_ns=%llu matching_allocs=%llu\n",blocks,batches,batches*packets_per_batch,size_t(batches)*packets_per_batch*payload_size,elapsed_ns,sorted_ns[199],sorted_ns[379],total_allocs);
  close(tx);
}
