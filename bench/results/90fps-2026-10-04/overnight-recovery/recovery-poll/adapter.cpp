// Host OS wait adapter only: actual deadline helper + shard_set, owned pipe.
// No sockets, XR session, accumulator instance, render loop or Pico.
#include "nack_deadline.h"
#include "shard_set.h"
#include <poll.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
int64_t now_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
wivrn::shard_set fixture(size_t count){
 wivrn::shard_set s(0);s.reset(7);
 for(size_t i=0;i<count;++i){if(i==1)continue;wivrn::to_headset::video_stream_data_shard v{};
  v.frame_idx=7;v.shard_idx=uint16_t(i);if(i==0)v.view_info.emplace();if(i+1==count)v.timing_info.emplace();s.insert(std::move(v),1);
 }return s;
}
std::optional<int64_t> deadline(wivrn::shard_set&s,int64_t now){
 std::vector<uint16_t>missing;
 return wivrn::nack_poll_deadline(now,s.last_shard,s.nack_last,s.nack_rounds,true,true,!s.empty(),s.complete(),[&]{s.missing_shards(missing,false);return !missing.empty();});
}
struct Pipe{int fds[2];Pipe(){if(pipe(fds))throw std::runtime_error("pipe");}~Pipe(){close(fds[0]);close(fds[1]);}};
void run(bool adaptive,bool next_arrival,size_t shards,int rep){
 auto s=fixture(shards);Pipe p;const auto start=Clock::now();const auto t0=now_ns();s.last_shard=t0;
 std::jthread producer;
 if(next_arrival)producer=std::jthread([&]{std::this_thread::sleep_until(start+20ms);char c=1;if(write(p.fds[1],&c,1)!=1)std::terminate();});
 const auto end=start+40ms;double first=-1;int wakes=0,events=0;
 while(Clock::now()<end){
  const auto now=now_ns();const auto rem=std::chrono::ceil<std::chrono::milliseconds>(end-Clock::now());
  if(rem<=0ms)break;
  auto timeout=rem;
  if(adaptive)timeout=std::min(timeout,wivrn::nack_poll_timeout(now,deadline(s,now),100ms));
  pollfd fd{p.fds[0],POLLIN,0};int r=poll(&fd,1,int(timeout.count()));if(r<0)throw std::runtime_error("poll");++wakes;
  bool arrived=r>0&&(fd.revents&POLLIN);if(arrived){char c;if(read(p.fds[0],&c,1)!=1)throw std::runtime_error("read");++events;}
  if(adaptive||arrived){const auto n=now_ns();const auto d=deadline(s,n);if(d&&*d<=n){
    first=double(n-t0)/1e6;++s.nack_rounds;s.nack_last=n;break;
  }}
 }
 if(adaptive&&first<0)throw std::runtime_error("adaptive missed request");
 if(!adaptive&&!next_arrival&&first>=0)throw std::runtime_error("unexpected arrival-only request");
 if(next_arrival&&!adaptive&&events!=1)throw std::runtime_error("missing arrival signal");
 printf("%d,%d,%zu,%d,%.6f,%d,%d\n",int(adaptive),int(next_arrival),shards,rep,first,wakes,events);
}
int main(){puts("adaptive,next_arrival,shards,rep,first_request_opportunity_ms,poll_returns,arrival_events");
 for(bool arrival:{true,false})for(size_t n:{size_t(3),size_t(521)})for(int rep=0;rep<10;++rep){
   if(rep%2){run(true,arrival,n,rep);run(false,arrival,n,rep);}else{run(false,arrival,n,rep);run(true,arrival,n,rep);}
 }
}
