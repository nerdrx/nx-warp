#include "wivrn_sockets.h"
#include <poll.h>
#include "wivrn_packets.h"
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <numeric>
#include <sys/socket.h>
#include <unistd.h>

using namespace wivrn;
using video_stream_data_shard = to_headset::video_stream_data_shard;
using tx_t = typed_socket<UDP, from_headset::packets, to_headset::packets>;
using rx_t = typed_socket<UDP, to_headset::packets, from_headset::packets>;
extern "C" int __real_sendmmsg(int, struct mmsghdr*, unsigned int, int);
int shim_mode = 0; // 1: cap next call to one message, 2: force EINTR once
extern "C" int __wrap_sendmmsg(int fd, struct mmsghdr* msg, unsigned int n, int flags) {
  if (shim_mode == 2) { shim_mode = 0; errno = EINTR; return -1; }
  if (shim_mode == 1) { shim_mode = 0; return __real_sendmmsg(fd, msg, n ? 1 : 0, flags); }
  return __real_sendmmsg(fd, msg, n, flags);
}

video_stream_data_shard shard(uint64_t id, std::vector<uint8_t>& bytes) {
  video_stream_data_shard s{}; s.stream_item_idx=0; s.frame_idx=id/64; s.shard_idx=id%64; s.payload=bytes; return s;
}
void verify(rx_t& rx, uint64_t id, const std::vector<uint8_t>& bytes) {
  auto v=rx.receive(); assert(v); auto* s=std::get_if<video_stream_data_shard>(&*v); assert(s);
  assert(s->frame_idx==id/64 && s->shard_idx==id%64 && s->payload.size()==bytes.size());
  assert(std::equal(bytes.begin(),bytes.end(),s->payload.begin()));
}
size_t batch(tx_t& tx, uint64_t start, size_t n, const std::vector<uint8_t>& bytes, std::vector<serialization_packet>& ps) { std::vector<std::vector<uint8_t>> copies(n, bytes);
  ps.resize(n); for(size_t i=0;i<n;i++) tx_t::serialize(ps[i], shard(start+i,copies[i]));
  return tx.send(ps);
}
void drain(rx_t& rx, uint64_t start, size_t n, std::vector<uint8_t>& b) { for(size_t i=0;i<n;i++) verify(rx,start+i,b); }

int main() {
  tx_t tx; rx_t rx;
  sockaddr_in6 addr{}; addr.sin6_family=AF_INET6; addr.sin6_addr=in6addr_loopback;
  rx.set_receive_buffer_size(4*1024*1024); tx.set_send_buffer_size(4*1024*1024); rx.bind(addr); socklen_t alen=sizeof(addr); getsockname(rx.get_fd(),(sockaddr*)&addr,&alen); tx.connect(in6addr_loopback,ntohs(addr.sin6_port));
  std::array<uint8_t,16> key{}; std::array<uint8_t,8> rh{}, sh{}; for(int i=0;i<16;i++) key[i]=i+1; for(int i=0;i<8;i++) rh[i]=sh[i]=0xa0+i;
  tx.set_aes_key_and_ivs(key,rh,sh); rx.set_aes_key_and_ivs(key,sh,rh);
  std::vector<uint8_t> payload(1200); for(size_t i=0;i<payload.size();i++) payload[i]=(i*29+17)&255;
  std::vector<serialization_packet> packets; packets.reserve(64);
  // Encrypted byte/order smoke for all requested widths.
  uint64_t id=0;
  for(size_t n: {1,8,64,128}) { auto sent=batch(tx,id,n,payload,packets); assert(sent>0); drain(rx,id,n,payload); id+=n; }
  std::cout << "encrypted_order_payload=PASS sizes=1,8,64,128 payload=1200\n";
  // Deterministic syscall behavior: partial positive return transmits one of eight;
  // API still reports aggregate bytes and silently omits seven.
  shim_mode=1; auto expected=batch(tx,id,8,payload,packets); assert(expected==8*1216); drain(rx,id,1,payload);
  pollfd pfd{rx.get_fd(),POLLIN,0}; assert(poll(&pfd,1,0)==0); std::cout << "partial_return=forced_1_of_8 delivered=1 reported_bytes="<<expected<<" expected_bytes="<<8*1216<<" tail_dropped=7\n"; id+=8;
  shim_mode=2; bool interrupted=false; try { batch(tx,id,8,payload,packets); } catch(const std::system_error& e) { interrupted=(e.code().value()==EINTR); }
  assert(interrupted); std::cout << "forced_EINTR=throws_EINTR\n";
  // Individual sends and batched sends, ABBA, one operation group at a time.
  auto run=[&](size_t n,bool use_batch,int groups)->double {
    double elapsed=0;
    for(int g=0;g<groups;g++) {
      uint64_t base=id; auto t0=std::chrono::steady_clock::now();
      if(use_batch) batch(tx,base,n,payload,packets);
      else for(size_t i=0;i<n;i++) { auto bytes=payload; tx.send(shard(base+i,bytes)); }
      elapsed+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count();
      drain(rx,base,n,payload); id+=n;
    }
    return elapsed/(groups*n);
  };
  for(size_t n: {1,8,64,128}) {
    for(int w=0;w<2;w++) { run(n,w,50); }
    std::array<char,4> order={'I','B','B','I'}; double sums[2]={}; int counts[2]={};
    for(int rep=0;rep<5;rep++) for(char mode:order) { int ix=mode=='B'; double us=run(n,ix,200); sums[ix]+=us; counts[ix]++; }
    std::cout<<"n="<<n<<" individual_us_per_datagram="<<sums[0]/counts[0]<<" batch_us_per_datagram="<<sums[1]/counts[1]<<" groups_per_block=200 blocks_per_mode=10 order=IBBI\n";
  }
}
