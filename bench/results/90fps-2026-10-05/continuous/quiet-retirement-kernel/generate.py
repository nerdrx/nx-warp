#!/usr/bin/env python3
import pathlib,sys,subprocess,hashlib,json
repo=pathlib.Path(sys.argv[1]).resolve(); out=pathlib.Path(sys.argv[2]).resolve(); out.mkdir(parents=True,exist_ok=True)
root=pathlib.Path(__file__).resolve().parent.parent
methods_dir=out/'methods'; poll_dir=out/'poll'
methods_dir.mkdir(exist_ok=True); poll_dir.mkdir(exist_ok=True)
subprocess.run(['python3',str(root/'quiet-retirement-service/extract.py'),str(repo),str(methods_dir)],check=True)
subprocess.run(['python3',str(root/'network-quiet-poll/extract.py'),str(repo),str(poll_dir)],check=True)
base=(methods_dir/'quiet-retirement-gate.cpp').read_text().replace('int main(){','int accumulator_gate_main(){',1)
base=base.replace('scenes::stream','wivrn::scenes::stream')
poll=(poll_dir/'poll-gate.cpp').read_text()
prefix=poll[:poll.index('static int checks=0, failures=0;')]
helpers=poll[poll.index('using udp_sender_t='):poll.index('int main(){')]
repo_src=repo/'client/scenes/stream_network.cpp'; src=repo_src.read_text(); sig='void scenes::stream::process_packets()'; start=src.index(sig); brace=src.index('{',start); dep=0
for end in range(brace,len(src)):
 if src[end]=='{': dep+=1
 elif src[end]=='}':
  dep-=1
  if dep==0: break
method=src[start:end+1].replace('void scenes::stream::process_packets()', 'void caller_projection::scenes::stream::process_packets()')
extra=r'''
using namespace std::chrono_literals;
namespace caller_projection { inline bool enabled=true; inline bool recovery_poll_enabled(){return enabled;} }
using udp_sender_t=typed_socket<UDP,from_headset::packets,to_headset::packets>;
using tcp_sender_t=typed_socket<TCP,from_headset::packets,to_headset::packets>;
class actual_session:public poll_projection{
public:
 fake_instance&clock;udp_sender_t udp_peer;tcp_sender_t tcp_peer;std::vector<int>requested_ms;std::vector<int>poll_returns;std::vector<int64_t>elapsed_ns;std::function<void()> stop_after_two;size_t calls=0;
 actual_session(poll_projection::stream_socket_t&&r,poll_projection::control_socket_t&&c,udp_sender_t&&u,tcp_sender_t&&t,fake_instance&cl):poll_projection(std::move(r),std::move(c)),clock(cl),udp_peer(std::move(u)),tcp_peer(std::move(t)){}
 template<class V,class S>int poll(V&&v,std::chrono::milliseconds maximum,S&&supplier){auto begin=std::chrono::steady_clock::now();int wanted=0;int ret=poll_projection::poll(std::forward<V>(v),maximum,[&]{auto w=std::min(maximum,supplier());wanted=int(w.count());return w;});auto end=std::chrono::steady_clock::now();auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count();poll_returns.push_back(ret);requested_ms.push_back(wanted);elapsed_ns.push_back(ns);clock.value+=ns;if(++calls==2&&stop_after_two)stop_after_two();return ret;}
 template<class V>int poll(V&&v,std::chrono::milliseconds maximum){return poll(std::forward<V>(v),maximum,[maximum]{return maximum;});}
};
namespace caller_projection { namespace scenes {
class stream {
public:
 fake_instance&instance;std::shared_mutex decoder_mutex;struct item{std::shared_ptr<shard_accumulator>decoder;};std::array<item,1>decoders;
 enum class state{streaming,shutdown};state state_=state::streaming;std::unique_ptr<actual_session>network_session;
 stream(std::shared_ptr<shard_accumulator>a):instance(a->instance),decoders{item{a}}{auto[u,up]=udp_pair();auto[c,cp]=tcp_pair();network_session=std::make_unique<actual_session>(std::move(u),std::move(c),std::move(up),std::move(cp),instance);network_session->stop_after_two=[this]{state_=state::shutdown;};a->decoder_->on_complete=[this]{state_=state::shutdown;};}
 template<class T>void operator()(T&&){}
 bool try_seamless_reconnect(){return false;}void exit(){state_=state::shutdown;}void process_packets();
};}}
__PROCESS__
static int64_t host_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static void integrated_case(bool recovery,bool deadline,bool native){
 auto a=std::make_shared<shard_accumulator>();std::shared_ptr<wivrn::scenes::stream>scene;XrTime start=host_ns()-10'000'000;setup(*a,scene,deadline,start);service_two_nack_rounds(*a,start);a->nxastc_codec=native;a->instance.value=std::max<XrTime>(a->instance.value,host_ns());caller_projection::enabled=recovery;caller_projection::scenes::stream loop(a);loop.process_packets();auto&s=*loop.network_session;bool expected=recovery&&deadline&&native;CHECK(a->window.front_index()==(expected?2u:0u));CHECK(a->decoder_->completed.size()==(expected?1u:0u));CHECK(a->nack_requests==2);CHECK(s.bytes_received_==0);CHECK(std::all_of(s.poll_returns.begin(),s.poll_returns.end(),[](int r){return r==0;}));CHECK(!s.requested_ms.empty());if(s.requested_ms.empty())return;if(expected){CHECK(s.requested_ms[0]>=0&&s.requested_ms[0]<100);CHECK(s.elapsed_ns[0]>=0);CHECK(loop.state_==caller_projection::scenes::stream::state::shutdown);}else{CHECK(s.requested_ms[0]==100);CHECK(s.requested_ms.size()==2);CHECK(loop.state_==caller_projection::scenes::stream::state::shutdown);}std::printf("case,%d,%d,%d,wait_ms=%d,elapsed_ns=%lld,polls=%zu,front=%llu,completed=%zu,received_bytes=%llu,poll_return=%d\n",recovery,deadline,native,s.requested_ms[0],(long long)s.elapsed_ns[0],s.calls,(unsigned long long)a->window.front_index(),a->decoder_->completed.size(),(unsigned long long)s.bytes_received_.load(),s.poll_returns.front());}
int main(){if(accumulator_gate_main())return 1;checks=0;failures=0;std::puts("kernel integration: exact process_packets + poll templates; host monotonic stand-in, no incoming packets");integrated_case(true,true,true);integrated_case(false,true,true);integrated_case(true,false,true);integrated_case(true,true,false);std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
'''.replace('__PROCESS__',method)
# Extend actual stub decoder only for end-to-end retirement stop callback.
base=base.replace('struct fake_decoder {','struct fake_decoder { std::function<void()> on_complete;').replace('completed.push_back(fb.frame_index);','completed.push_back(fb.frame_index);if(on_complete)on_complete();').replace('void report_nacks(XrTime){}','void report_nacks(XrTime){} bool is_nxastc_codec() const{return nxastc_codec;}')
cpp='#include <functional>\n'+base+'\n'+prefix+'\n'+helpers+'\n'+extra
(out/'kernel-gate.cpp').write_text(cpp)
prov={'head':subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip(),'generated_cpp_sha256':hashlib.sha256(cpp.encode()).hexdigest(),'process_packets':{'lines':[src[:start].count('\n')+1,src[:end+1].count('\n')+1],'production_sha256':hashlib.sha256(src[start:end+1].encode()).hexdigest(),'projected_sha256':hashlib.sha256(method.encode()).hexdigest()},'poll_cpp_sha256':hashlib.sha256((poll_dir/'poll-gate.cpp').read_bytes()).hexdigest(),'accumulator_cpp_sha256':hashlib.sha256((methods_dir/'quiet-retirement-gate.cpp').read_bytes()).hexdigest(),'scope':'Actual poll template and Linux poll with typed loopback sockets; exact process_packets body; extracted six actual accumulator methods. Constructor/push_packet/XR/decoder/socket packet processing excluded; clock is monotonic stand-in advanced by measured poll wait.'}
(out/'provenance.json').write_text(json.dumps(prov,indent=2)+'\n')
