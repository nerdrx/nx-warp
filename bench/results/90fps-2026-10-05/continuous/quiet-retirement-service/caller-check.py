#!/usr/bin/env python3
import pathlib,sys,hashlib,json
repo=pathlib.Path(sys.argv[1]); base=pathlib.Path(sys.argv[2]); out=pathlib.Path(sys.argv[3]);out.mkdir(parents=True,exist_ok=True)
p=repo/'client/scenes/stream_network.cpp';text=p.read_text();sig='void scenes::stream::process_packets()';start=text.index(sig);brace=text.index('{',start);depth=0
for end in range(brace,len(text)):
    if text[end]=='{':depth+=1
    elif text[end]=='}':
        depth-=1
        if depth==0:break
method=text[start:end+1]
assert base.read_text().count('int main(){') == 1
cpp=base.read_text().replace('int main(){','int method_gate_main(){',1)
cpp=cpp.replace('void report_nacks(XrTime){}','void report_nacks(XrTime){}\n bool is_nxastc_codec() const{return nxastc_codec;}')
extra=r'''
#include <functional>
#include <shared_mutex>
#include <stdexcept>
namespace caller_projection {
static bool enabled=true;
static bool recovery_poll_enabled(){return enabled;}
namespace scenes { struct stream; }
struct session {
 fake_instance &clock;
 std::vector<int64_t> waits;
 std::function<void()> stop;
 template<class Visitor,class Supplier> void poll(Visitor&,std::chrono::milliseconds maximum,Supplier&& supplier){
  auto wait=std::min(maximum,supplier());waits.push_back(wait.count());
  clock.value+=wait.count()*1'000'000;
  if(waits.size()==2)stop();
  if(waits.size()>2)throw std::runtime_error("unbounded projected caller");
 }
 template<class Visitor> void poll(Visitor&v,std::chrono::milliseconds wait){poll(v,wait,[wait]{return wait;});}
};
namespace scenes {
struct stream {
 fake_instance &instance;
 std::shared_mutex decoder_mutex;
 struct item {std::shared_ptr<shard_accumulator> decoder;};
 std::array<item,1> decoders;
 enum class state {streaming,shutdown};state state_=state::streaming;
 std::unique_ptr<session> network_session;
 stream(std::shared_ptr<shard_accumulator>a):instance(a->instance),decoders{item{a}},network_session(std::make_unique<session>(session{instance,{},{}})){network_session->stop=[this]{state_=state::shutdown;};}
 bool try_seamless_reconnect(){return false;}
 void exit(){state_=state::shutdown;}
 void process_packets();
};
}
__EXACT_PROCESS_PACKETS__
}
static void caller_case(bool recovery,bool deadline,bool native){
 auto a=std::make_shared<shard_accumulator>();std::shared_ptr<wivrn::scenes::stream>s;
 setup(*a,s,deadline);service_two_nack_rounds(*a,1'000'000'000);a->nxastc_codec=native;
 caller_projection::enabled=recovery;caller_projection::scenes::stream loop(a);loop.process_packets();
 auto&waits=loop.network_session->waits;CHECK(waits.size()==2);
 CHECK(waits[0]==(recovery&&deadline&&native?18:100));CHECK(waits[1]==100);
 CHECK(a->window.front_index()==(recovery&&deadline&&native?2u:0u));
 CHECK(a->decoder_->completed.size()==(recovery&&deadline&&native?1u:0u));CHECK(a->nack_requests==2);
 std::printf("caller,%d,%d,%d,%lld,%llu,%zu\n",recovery,deadline,native,(long long)waits[0],(unsigned long long)a->window.front_index(),a->decoder_->completed.size());
}
int main(){
 int base_result=method_gate_main();if(base_result)return base_result;
 checks=0;failures=0;
 std::puts("kind,recovery_poll,deadline,native,first_wait_ms,front,decoder_stub_completed");
 caller_case(true,true,true);caller_case(false,true,true);caller_case(true,false,true);caller_case(true,true,false);
 std::printf("caller_checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
'''.replace('__EXACT_PROCESS_PACKETS__',method)
(out/'caller-gate.cpp').write_text(cpp+extra)
(out/'caller-provenance.json').write_text(json.dumps({'source':str(p),'source_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'method_lines':[text[:start].count('\n')+1,text[:end+1].count('\n')+1],'method_sha256':hashlib.sha256(method.encode()).hexdigest(),'base_cpp_sha256':hashlib.sha256(base.read_bytes()).hexdigest(),'scope':'Exact process_packets body with virtual clock and session; no actual thread/sockets/XR/decoder.'},indent=2)+'\n')
