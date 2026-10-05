#!/usr/bin/env python3
import hashlib,pathlib,re,subprocess,sys
repo=pathlib.Path(sys.argv[1]).resolve();out=pathlib.Path(sys.argv[2]).resolve();out.mkdir(parents=True,exist_ok=True)
accpath=repo/'client/decoder/shard_accumulator.cpp'; at=accpath.read_text(); ah=repo/'client/decoder/shard_accumulator.h'; aht=ah.read_text()
client=repo/'client/wivrn_client.h'; ct=client.read_text()
def extract(src,sig,prefix=None):
 s=src.index(sig); s=src.rfind(prefix,0,s) if prefix else s; b=src.index('{',s); d=0
 for e in range(b,len(src)):
  if src[e]=='{':d+=1
  elif src[e]=='}':
   d-=1
   if d==0:return src[s:e+1],s,e+1
 raise RuntimeError(sig)
methods=[]; spans=[]
for sig in ['std::optional<XrTime> shard_accumulator::next_nack_deadline(XrTime now)','void shard_accumulator::poll_nacks(XrTime now)','void shard_accumulator::try_nack(XrTime now)']:
 body,s,e=extract(at,sig);methods.append(body);spans.append((s,e))
poll,ps,pe=extract(ct,'int poll(T && visitor, std::chrono::milliseconds max_timeout, TimeoutSupplier && timeout_supplier)','\ttemplate <typename T, typename TimeoutSupplier>')
window_alias=re.search(r'\tusing window_t\s*=\s*[^;]+;',aht).group(0)
licenses=at[:at.index('#include')].strip()+'\n\n'+ct[:ct.index('#pragma once')].strip()+'\n\n'
pre=r'''#include "fec.h"
#include "frame_window.h"
#include "nack_deadline.h"
#include "shard_set.h"
#include "shard_history.h"
#include "wivrn_client.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <shared_mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>
#include <spdlog/spdlog.h>
using namespace std::chrono_literals;
using namespace wivrn;
static int checks=0, failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::printf("FAIL:%d %s\n",__LINE__,#x);}}while(false)
namespace wivrn {
namespace application { struct config { bool shard_retransmit=true; }; inline config current{}; inline config &get_config(){return current;} }
namespace scenes { struct stream { std::function<void(const from_headset::nack&)> send; void send_nack(const from_headset::nack& n){send(n);} }; }
class shard_accumulator {
public: using shard_set=wivrn::shard_set; __WINDOW_ALIAS__
 bool nxastc_codec=true; std::weak_ptr<scenes::stream> weak_scene; window_t window{shard_set{2}};
 std::vector<uint16_t> nack_scratch; uint64_t nack_requests=0,nack_shards=0; std::atomic<uint64_t> nack_shards_total=0;
 std::optional<XrTime> next_nack_deadline(XrTime); void poll_nacks(XrTime); void try_nack(XrTime); void report_nacks(XrTime){}
};
__ACC_METHODS__
}
class poll_projection {
public:
 using control_socket_t=typed_socket<TCP,to_headset::packets,from_headset::packets>;
 using stream_socket_t=typed_socket<UDP,to_headset::packets,from_headset::packets>;
 stream_socket_t stream;control_socket_t control;control_socket_t secondary{-1};
 std::atomic<uint64_t> bytes_received_{0};std::shared_mutex secondary_mutex;uint64_t secondary_generation=0;std::atomic<bool> secondary_up{false};
 struct selector_type{bool control_up()const{return true;}} selector;
 poll_projection(stream_socket_t&&s,control_socket_t&&c):stream(std::move(s)),control(std::move(c)){}
 void poll_secondary_pending(auto&&){} void on_primary_received(bool){} bool on_control_send_error(const std::exception&){return false;} void on_stream_send_error(const std::exception&){} void drop_secondary(std::string_view){secondary_up=false;} void update_paths(){}
 __POLL__
};
'''.replace('__WINDOW_ALIAS__',window_alias).replace('__ACC_METHODS__','\n'.join(methods)).replace('__POLL__',poll)
post=r'''
using namespace wivrn;
using data_shard=to_headset::video_stream_data_shard;
using data_tx=typed_socket<UDP,from_headset::packets,to_headset::packets>;
using ctl_tx=typed_socket<TCP,from_headset::packets,to_headset::packets>;
struct sockets {poll_projection::stream_socket_t rx;data_tx tx;poll_projection::control_socket_t crx;ctl_tx ctx;};
static sockets make_sockets(){UDP rx,tx; sockaddr_in6 a{};a.sin6_family=AF_INET6;a.sin6_addr=in6addr_loopback;a.sin6_port=0;rx.bind(a);tx.bind(a);sockaddr_in6 actual{},other{};socklen_t n=sizeof(actual);getsockname(rx.get_fd(),(sockaddr*)&actual,&n);n=sizeof(other);getsockname(tx.get_fd(),(sockaddr*)&other,&n);rx.connect(in6addr_loopback,ntohs(other.sin6_port));tx.connect(in6addr_loopback,ntohs(actual.sin6_port));TCPListener l(0);sockaddr_in6 ca{};n=sizeof(ca);getsockname(l.get_fd(),(sockaddr*)&ca,&n);TCP ctx(in6addr_loopback,ntohs(ca.sin6_port));auto [crx,peer]=l.accept<TCP>();return{poll_projection::stream_socket_t(std::move(rx)),data_tx(std::move(tx)),poll_projection::control_socket_t(std::move(crx)),ctl_tx(std::move(ctx))};}
static int64_t ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static std::array<std::array<std::array<uint8_t,16>,4>,100> payloads{};
static data_shard make_shard(uint64_t f,uint16_t i,bool end=false){data_shard d{};d.stream_item_idx=2;d.frame_idx=f;d.shard_idx=i;auto &bytes=payloads[f%100][i%4];bytes.fill(uint8_t(i+10));d.payload=bytes;if(i==0)d.view_info=data_shard::view_info_t{.display_time=42};if(end)d.timing_info=data_shard::timing_info_t{1,2,3,4};return d;}
static std::vector<uint16_t> requested(const from_headset::nack& n){std::vector<uint16_t> v;for(size_t b=0;b<n.bitmap.size();++b)for(int j=0;j<8;++j)if(n.bitmap[b]&(1u<<j))v.push_back(uint16_t(n.first_shard_idx+b*8+j));return v;}
int main(){
 std::puts("event,mono_ns,frame,index,count");auto sock=make_sockets();poll_projection session(std::move(sock.rx),std::move(sock.crx));shard_accumulator acc;auto scene=std::make_shared<scenes::stream>();scene->send=[&](auto&n){session.stream.send(from_headset::nack(n));std::printf("nack_send,%lld,40,%u,%zu\n",(long long)ns(),unsigned(n.first_shard_idx),requested(n).size());};acc.weak_scene=scene;
 bool completion_logged=false;auto visitor=[&](auto&& p){using P=std::decay_t<decltype(p)>;if constexpr(std::is_same_v<P,data_shard>){auto f=p.frame_idx;auto i=p.shard_idx;auto *q=acc.window.slot(f,[](shard_set&){});q->insert(std::move(p),ns());std::printf("client_shard,%lld,%llu,%u,0\n",(long long)ns(),(unsigned long long)f,unsigned(i));if(f==40&&q->complete()&&!completion_logged){completion_logged=true;std::printf("window_complete,%lld,40,3,1\n",(long long)ns());}}};
 // Sender's real retained-history format and decoder create the wire reply.
 shard_history hist;hist.set_enabled(true);std::array<data_shard,4> frame{make_shard(40,0),make_shard(40,1),make_shard(40,2),make_shard(40,3,true)};
 for(auto&d:frame){std::vector<uint8_t> blob;fec::encode_blob(d,blob);hist.push(40,d.shard_idx,blob,true);}hist.end_frame(40,4);
 sock.tx.send(data_shard(frame[0]));sock.tx.send(data_shard(frame[3]));std::this_thread::sleep_for(3ms);
 for(int i=0;i<3;++i){session.poll(visitor,10ms,[&]{return 2ms;});auto *q=acc.window.slot(40,[](shard_set&){});if(q->shards().size()>=4&&q->shards()[0]&&q->shards()[3])break;}auto &set=*acc.window.slot(40,[](shard_set&){});CHECK(set.shards().size()>=4&&set.shards()[0]&&set.shards()[3]&&!set.complete());if(set.shards().size()<4||!set.shards()[0]||!set.shards()[3])return 2;std::vector<uint16_t> holes;set.missing_shards(holes,false);std::printf("initial_hole,%lld,40,0,%zu\n",(long long)ns(),holes.size());CHECK(holes.size()==2&&holes[0]==1&&holes[1]==2);CHECK(set.feedback.stream_index==2);CHECK(set.shards()[0]->view_info.has_value()&&set.shards()[3]->timing_info.has_value());
 auto t=ns();auto deadline=acc.next_nack_deadline(t);CHECK(deadline.has_value());int pollret=session.poll(visitor,100ms,[&]{auto n=ns();return nack_poll_timeout(n,acc.next_nack_deadline(n),100ms);});std::printf("poll_return,%lld,40,0,%d\n",(long long)ns(),pollret);acc.poll_nacks(ns());
 pollfd pf{.fd=sock.tx.get_fd(),.events=POLLIN};CHECK(::poll(&pf,1,100)==1);auto packet=sock.tx.receive();CHECK(packet.has_value());auto nack=std::get<from_headset::nack>(std::move(*packet));auto want=requested(nack);CHECK(nack.stream_index==2&&nack.frame_idx==40&&want.size()==2&&want[0]==1&&want[1]==2);std::printf("peer_nack,%lld,40,%u,%zu\n",(long long)ns(),unsigned(nack.first_shard_idx),want.size());
 std::vector<shard_history::hit> hits;auto count=hist.collect(nack.frame_idx,nack.first_shard_idx,nack.bitmap,64,hits);CHECK(count==2&&hits.size()==2);for(auto&h:hits){auto d=fec::decode_blob(2,40,h.shard_idx,h.blob);CHECK(std::equal(d.payload.begin(),d.payload.end(),frame[h.shard_idx].payload.begin()));sock.tx.send(std::move(d));}std::printf("peer_repair_sent,%lld,40,1,%zu\n",(long long)ns(),hits.size());
 for(int i=0;i<3&&!set.complete();++i)session.poll(visitor,10ms,[&]{return 2ms;});CHECK(set.complete()&&completion_logged);CHECK(set.shards()[0]->view_info.has_value()&&set.shards()[3]->timing_info.has_value());for(uint16_t i=1;i<=2;++i)CHECK(set.shards()[i]->payload.size()==frame[i].payload.size()&&std::equal(set.shards()[i]->payload.begin(),set.shards()[i]->payload.end(),frame[i].payload.begin()));
 // Parity arrives and reconstructs the only interior hole before the due NACK check.
 shard_accumulator parity_acc;auto ps=std::make_shared<scenes::stream>();int parity_nacks=0;ps->send=[&](auto&){++parity_nacks;};parity_acc.weak_scene=ps;auto p0=make_shard(45,0),p1=make_shard(45,1),p2=make_shard(45,2,true);fec::group_builder gb;gb.set_layout(2,1);gb.reset(2,45);gb.add(p0);gb.add(p1);auto pb=gb.take();CHECK(pb.has_value());auto &p_set=*parity_acc.window.slot(45,[](shard_set&){});p_set.insert(data_shard(p0),3000);p_set.insert(data_shard(p2),3001);p_set.parity.push_back(*pb);auto fixed=p_set.reconstruct(p_set.parity[0],3002);CHECK(fixed&&*fixed==1&&p_set.complete());p_set.parity.clear();parity_acc.try_nack(3001+nack_quiet_period_ns);CHECK(parity_nacks==0);std::printf("parity_suppresses_nack,%lld,45,0,0\n",(long long)ns());
 // Two rounds and unknown newest tail use the exact accumulator methods without the socket seam.
 auto direct_scene=std::make_shared<scenes::stream>();int direct_nacks=0;direct_scene->send=[&](auto&){++direct_nacks;};shard_accumulator two;two.weak_scene=direct_scene;auto add=[&](uint64_t f,uint16_t i,int64_t at,bool end=false){auto d=make_shard(f,i,end);two.window.slot(f,[](shard_set&){})->insert(std::move(d),at);};add(50,0,1000);add(50,3,1001,true);add(51,0,1002);two.try_nack(1001+nack_quiet_period_ns);two.try_nack(1001+2*nack_quiet_period_ns);two.try_nack(1001+3*nack_quiet_period_ns);CHECK(direct_nacks==2&&two.window.slot(50,[](shard_set&){})->nack_rounds==2);std::printf("round_limit,%lld,50,2,%d\n",(long long)ns(),direct_nacks);
 shard_accumulator unknown;auto us=std::make_shared<scenes::stream>();int unknown_nacks=0;us->send=[&](auto&){++unknown_nacks;};unknown.weak_scene=us;auto u0=make_shard(60,0);auto u1=make_shard(60,1);unknown.window.slot(60,[](shard_set&){})->insert(std::move(u0),2000);unknown.window.slot(60,[](shard_set&){})->insert(std::move(u1),2001);unknown.try_nack(2001+nack_quiet_period_ns);CHECK(unknown_nacks==0);std::printf("newest_unknown_tail,%lld,60,0,0\n",(long long)ns());
 CHECK(acc.nack_requests==1&&acc.nack_shards==2);std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
'''
cpp=licenses+pre+'\n'+post
(out/'loopback-gate.cpp').write_text(cpp)
methods_meta=''.join(f'method_{i+1}_lines={at[:s].count(chr(10))+1}-{at[:e].count(chr(10))+1}\nmethod_{i+1}_sha256={hashlib.sha256(m.encode()).hexdigest()}\n' for i,(m,(s,e)) in enumerate(zip(methods,spans)))
(out/'provenance.txt').write_text(f'HEAD={subprocess.check_output(["git","-C",str(repo),"rev-parse","HEAD"],text=True).strip()}\naccumulator={accpath}\naccumulator_sha256={hashlib.sha256(accpath.read_bytes()).hexdigest()}\nclient={client}\nclient_sha256={hashlib.sha256(client.read_bytes()).hexdigest()}\npoll_lines={ct[:ps].count(chr(10))+1}-{ct[:pe].count(chr(10))+1}\npoll_sha256={hashlib.sha256(poll.encode()).hexdigest()}\nwindow_alias={window_alias.strip()}\n'+methods_meta)
