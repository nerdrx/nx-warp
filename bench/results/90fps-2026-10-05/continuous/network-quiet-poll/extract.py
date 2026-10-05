#!/usr/bin/env python3
import hashlib, pathlib, re, sys
repo=pathlib.Path(sys.argv[1]).resolve(); out=pathlib.Path(sys.argv[2]).resolve()
p=repo/'client/wivrn_client.h'; src=p.read_text()
sig='int poll(T && visitor, std::chrono::milliseconds max_timeout, TimeoutSupplier && timeout_supplier)'
start=src.index(sig); start=src.rfind('\ttemplate <typename T, typename TimeoutSupplier>',0,start); brace=src.index('{',start); depth=0
for end in range(brace,len(src)):
    if src[end]=='{': depth+=1
    elif src[end]=='}':
        depth-=1
        if depth==0: break
method=src[start:end+1]
cpp=r'''#include "wivrn_sockets.h"
#include "wivrn_packets.h"
#include "nack_deadline.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <poll.h>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <spdlog/spdlog.h>
using namespace std::chrono_literals;
using namespace wivrn;
struct selector_stub { bool control_up() const { return true; } };
class poll_projection {
public:
 using control_socket_t=typed_socket<TCP, to_headset::packets, from_headset::packets>;
 using stream_socket_t=typed_socket<UDP, to_headset::packets, from_headset::packets>;
 stream_socket_t stream; control_socket_t control; control_socket_t secondary{-1};
 std::atomic<uint64_t> bytes_received_{0}; std::shared_mutex secondary_mutex;
 uint64_t secondary_generation=0; std::atomic<bool> secondary_up{false}; selector_stub selector;
 poll_projection(stream_socket_t &&s,control_socket_t &&c):stream(std::move(s)),control(std::move(c)){}
 void poll_secondary_pending(auto&&){}
 void on_primary_received(bool){}
 bool on_control_send_error(const std::exception&){return false;}
 void on_stream_send_error(const std::exception&){}
 void drop_secondary(std::string_view) { secondary_up=false; }
 void update_paths(){}
 __EXACT_POLL_METHOD__
};
static int checks=0, failures=0;
#define CHECK(x) do { ++checks; if(!(x)){++failures;std::printf("FAIL:%d %s\n",__LINE__,#x);} }while(false)
using udp_sender_t=typed_socket<UDP, from_headset::packets, to_headset::packets>;
using tcp_sender_t=typed_socket<TCP, from_headset::packets, to_headset::packets>;
static std::pair<poll_projection::stream_socket_t,udp_sender_t> udp_pair(){
 wivrn::UDP rx; sockaddr_in6 bindaddr{};bindaddr.sin6_family=AF_INET6;bindaddr.sin6_addr=in6addr_loopback;bindaddr.sin6_port=0;rx.bind(bindaddr);
 sockaddr_in6 actual{};socklen_t n=sizeof(actual);getsockname(rx.get_fd(),(sockaddr*)&actual,&n);
 wivrn::UDP tx;tx.connect(in6addr_loopback,ntohs(actual.sin6_port));
 return {poll_projection::stream_socket_t(std::move(rx)),udp_sender_t(std::move(tx))};
}
static std::pair<poll_projection::control_socket_t,tcp_sender_t> tcp_pair(){
 TCPListener listener(0);sockaddr_in6 addr{};socklen_t n=sizeof(addr);getsockname(listener.get_fd(),(sockaddr*)&addr,&n);
 wivrn::TCP tx(in6addr_loopback,ntohs(addr.sin6_port));auto [rx,peer]=listener.accept<wivrn::TCP>();
 return {poll_projection::control_socket_t(std::move(rx)),tcp_sender_t(std::move(tx))};
}
int main(){
 auto [rx,udp_tx]=udp_pair();auto [ctrl_rx,tcp_tx]=tcp_pair();poll_projection s(std::move(rx),std::move(ctrl_rx));
 std::puts("case,return,received_at_supplier,packets_seen,elapsed_ns,supplier_calls,callback_count");
 size_t seen=0;auto visitor=[&](auto&& p){using P=std::decay_t<decltype(p)>;if constexpr(std::is_same_v<P,to_headset::stream_padding>)++seen;else if constexpr(std::is_same_v<P,to_headset::server_message>)++seen;};
 // Quiet path: actual ::poll waits on real typed-socket fds; supplier caps timeout.
 auto t0=std::chrono::steady_clock::now();int suppliers=0;
 int r=s.poll(visitor,100ms,[&]{++suppliers;return 3ms;});auto quiet_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count();
 CHECK(r==0&&suppliers==1&&seen==0);std::printf("quiet_wake,%d,%zu,%zu,%lld,%d,0\n",r,seen,seen,(long long)quiet_ns,suppliers);
 // The production helper's overdue path is compared with a local-only 0ms variant.
 auto mono_ns=[](){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();};
 auto candidate_timeout=[&](int64_t now,int64_t due){if(now>=due)return 0ms;return wivrn::nack_poll_timeout(now,due,100ms);};
 std::puts("overdue_case,trial,elapsed_ns,poll_return,post_poll_service_calls");
 for(int trial=0;trial<14;++trial)for(int variant=0;variant<2;++variant){int which=(trial%2)?1-variant:variant;int64_t now=mono_ns(),due=now-1000000;int serviced=0;auto begin=std::chrono::steady_clock::now();
  int pr=s.poll(visitor,100ms,[&]{int64_t t=mono_ns();return which?wivrn::nack_poll_timeout(t,due,100ms):candidate_timeout(t,due);});
  auto poll_done=std::chrono::steady_clock::now();if(mono_ns()>=due)++serviced;auto poll_us=std::chrono::duration_cast<std::chrono::nanoseconds>(poll_done-begin).count();
  CHECK(pr==0&&serviced==1);std::printf("%s,%d,%lld,%d,%d\n",which?"baseline_1ms":"local_candidate_0ms",trial,(long long)poll_us,pr,serviced);
 }
 // UDP burst lands in real kernel receive batch; next poll drains typed_socket's remainder
 // before evaluating its timeout supplier. No assumption about Wi-Fi or RF.
 for(int i=0;i<8;++i)udp_tx.send(to_headset::stream_padding{});
 std::this_thread::sleep_for(5ms);s.poll(visitor,100ms,[&]{return 20ms;});CHECK(seen>=1);
 size_t seen_at_supplier=0;int r2=s.poll(visitor,100ms,[&]{seen_at_supplier=seen;return 0ms;});
 CHECK(r2==0&&seen_at_supplier>=2);std::printf("udp_pending_drain,%d,%zu,%zu,,1,%d\n",r2,seen_at_supplier,seen,int(seen_at_supplier));
 // Real TCP control receive over the connected loopback typed socket.
#ifndef SKIP_TCP_IO
 tcp_tx.send(to_headset::server_message{.kind=to_headset::server_message::kind::toast,.msg="gate"});
 int control_polls=0;while(seen<9&&control_polls++<4)s.poll(visitor,20ms,[&]{return 20ms;});
 CHECK(seen>=9);std::printf("tcp_control,%d,%zu,%zu,,%d,0\n",control_polls,seen,seen,control_polls);
#endif
 std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
'''.replace('__EXACT_POLL_METHOD__',method)
(out/'poll-gate.cpp').write_text(cpp)
(out/'provenance.txt').write_text(f'head={__import__("subprocess").check_output(["git","-C",str(repo),"rev-parse","HEAD"],text=True).strip()}\nsource={p}\nsource_sha256={hashlib.sha256(p.read_bytes()).hexdigest()}\nmethod_lines={src[:start].count(chr(10))+1}-{src[:end+1].count(chr(10))+1}\nmethod_sha256={hashlib.sha256(method.encode()).hexdigest()}\n')
