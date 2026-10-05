#!/usr/bin/env python3
import hashlib, json, pathlib, re, sys
repo=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2]); out.mkdir(parents=True,exist_ok=True)
source=repo/'client/decoder/shard_accumulator.cpp'; text=source.read_text()
header=repo/'client/decoder/shard_accumulator.h'; htext=header.read_text()
network=repo/'client/scenes/stream_network.cpp'; ntext=network.read_text()
process_sig='void scenes::stream::process_packets()'; ps=ntext.index(process_sig); pb=ntext.index('{',ps); pd=0
for pe in range(pb,len(ntext)):
    if ntext[pe]=='{': pd+=1
    elif ntext[pe]=='}':
        pd-=1
        if pd==0: break
caller=ntext[ps:pe+1]
assert 'item.decoder->next_poll_deadline(now)' in caller
assert 'network_session->poll(*this, std::chrono::milliseconds(100), timeout)' in caller
assert caller.index('network_session->poll(*this, std::chrono::milliseconds(100), timeout)') < caller.index('item.decoder->poll_nacks(now)')
signatures=[
 'std::optional<XrTime> shard_accumulator::next_nack_deadline(XrTime now)',
 'std::optional<XrTime> shard_accumulator::next_poll_deadline(XrTime now)',
 'void shard_accumulator::poll_nacks(XrTime now)',
 'void shard_accumulator::try_nack(XrTime now)',
 'void shard_accumulator::pump(XrTime now)',
 'shard_accumulator::window_t::step shard_accumulator::try_submit_front(shard_set & current)',
]
methods=[]; provenance={}
for sig in signatures:
    start=text.index(sig); brace=text.index('{',start); depth=0
    for end in range(brace,len(text)):
        if text[end]=='{': depth+=1
        elif text[end]=='}':
            depth-=1
            if depth==0: break
    body=text[start:end+1]; methods.append(body)
    provenance[sig]={'lines':[text[:start].count('\n')+1,text[:end+1].count('\n')+1], 'sha256':hashlib.sha256(body.encode()).hexdigest()}
alias=re.search(r'\tusing window_t\s*=\s*[^;]+;',htext).group(0)
head=r'''#include "frame_window.h"
#include "nack_deadline.h"
#include "shard_set.h"
#include "wivrn_packets.h"
#include <memory>
#include <cstdio>
#include <vector>
#include <atomic>
#include <spdlog/spdlog.h>
namespace wivrn {
namespace application { struct config { bool shard_retransmit=true; }; inline config current{}; inline config& get_config(){return current;} }
namespace scenes { struct stream { std::vector<from_headset::nack> requests; XrDuration period=11'111'111; void send_nack(const from_headset::nack&n){requests.push_back(n);} XrDuration display_period_ns()const{return period;} }; }
struct fake_instance { XrTime value=1; XrTime now()const{return value;} };
struct fake_decoder {
 std::vector<uint64_t> completed; size_t bytes=0;
 void push_data(std::span<const std::span<const uint8_t>> payload,uint64_t,bool){for(auto p:payload)bytes+=p.size();}
 void frame_completed(const from_headset::feedback&fb,const to_headset::video_stream_data_shard::view_info_t&){completed.push_back(fb.frame_index);}
};
class shard_accumulator {
public:
 using shard_set=wivrn::shard_set; using data_shard=to_headset::video_stream_data_shard;
 __ALIAS__
 window_t window{shard_set{2}}; fake_instance instance;
 std::shared_ptr<fake_decoder> decoder_=std::make_shared<fake_decoder>();
 std::weak_ptr<scenes::stream> weak_scene; bool astc_deadline_enabled=false; bool nxastc_codec=true;
 std::vector<uint16_t> nack_scratch; uint64_t nack_requests=0,nack_shards=0;
 std::atomic<uint64_t> nack_shards_total=0;
 void report_nacks(XrTime){}
 std::optional<XrTime> next_nack_deadline(XrTime); std::optional<XrTime> next_poll_deadline(XrTime);
 void poll_nacks(XrTime); void try_nack(XrTime); void pump(XrTime);
 window_t::step try_submit_front(shard_set&);
 std::vector<uint64_t> feedback_frames;
 void send_feedback(from_headset::feedback&fb){feedback_frames.push_back(fb.frame_index);}
};
static void debug_why_not_sent(const shard_accumulator::shard_set&){}
'''.replace('__ALIAS__',alias)
foot=r'''
}
using namespace wivrn;
static int checks=0, failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::printf("FAIL:%d %s\n",__LINE__,#x);}}while(false)
static void add(shard_accumulator&a,uint64_t frame,uint16_t index,XrTime now,bool tail){
 auto*s=a.window.slot(frame,[](auto&){});CHECK(s);if(!s)return;
 to_headset::video_stream_data_shard p{};p.stream_item_idx=2;p.frame_idx=frame;p.shard_idx=index;
 if(index==0)p.view_info=to_headset::video_stream_data_shard::view_info_t{};
 if(tail)p.timing_info=to_headset::video_stream_data_shard::timing_info_t{1,2,3,4};
 static std::array<uint8_t,4> bytes{1,2,3,4};p.payload=bytes;s->insert(std::move(p),now);
 if(s->complete()) a.window.note_complete(frame);
 a.instance.value=now;a.pump(now);
}
static void setup(shard_accumulator&a,std::shared_ptr<scenes::stream>&scene,bool deadline=true,XrTime t=1'000'000'000){
 scene=std::make_shared<scenes::stream>();a.weak_scene=scene;a.astc_deadline_enabled=deadline;
 // Old frame has an interior hole and a real terminal shard; next frame is complete.
 add(a,0,0,t,false);add(a,0,2,t+1,true);add(a,1,0,t+2,false);add(a,1,1,t+3,true);
 CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());
}
static void service_two_nack_rounds(shard_accumulator&a,XrTime t){
 const XrTime quiet=nack_quiet_period_ns;
 const XrTime active=t+1;
 a.instance.value=active+quiet;a.poll_nacks(a.instance.value);CHECK(a.nack_requests==1);
 a.instance.value=active+2*quiet;a.poll_nacks(a.instance.value);CHECK(a.nack_requests==2);
 CHECK(a.window.front().nack_rounds==2);
}
static void test_retirement(){
 shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true);const XrTime t=1'000'000'000;const XrDuration p=s->period;
 auto before=a.next_poll_deadline(t+3);CHECK(before&&*before==t+1+nack_quiet_period_ns);
 service_two_nack_rounds(a,t);auto due=a.next_poll_deadline(t+3+2*nack_quiet_period_ns);CHECK(due&&*due==t+2*p);
 a.instance.value=*due-1;a.poll_nacks(a.instance.value);CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());CHECK(a.next_poll_deadline(*due-1)==due);
 a.instance.value=*due;a.poll_nacks(*due);CHECK(a.window.front_index()==2);CHECK(a.decoder_->completed.size()==1);CHECK(a.decoder_->completed[0]==1);
 CHECK(a.feedback_frames.size()==2);CHECK(!a.next_poll_deadline(*due));
 // A later quiet poll cannot repeat retirement or redeliver the already-pumped frame.
 a.instance.value=*due+100'000'000;a.poll_nacks(a.instance.value);CHECK(a.window.front_index()==2);CHECK(a.decoder_->completed.size()==1);
}
static void test_controls(){
 const XrTime t=2'000'000'000;
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,false,t);CHECK(a.next_poll_deadline(t+2*s->period)==a.next_nack_deadline(t+2*s->period));a.instance.value=t+2*s->period;a.poll_nacks(a.instance.value);CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);application::current.shard_retransmit=false;CHECK(a.next_poll_deadline(t+3)==std::optional<XrTime>(t+2*s->period));a.instance.value=t+2*s->period;a.poll_nacks(a.instance.value);CHECK(a.window.front_index()==2);CHECK(a.decoder_->completed.size()==1);application::current.shard_retransmit=true;}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);a.nxastc_codec=false;CHECK(!a.next_poll_deadline(t));}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);s.reset();a.weak_scene.reset();CHECK(!a.next_poll_deadline(t+3));}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);s->period=0;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));s->period=-1;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));s->period=std::numeric_limits<XrDuration>::max();CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);auto*front=&a.window.front();front->feedback.received_first_packet=0;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));front->feedback.received_first_packet=-1;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);auto*front=&a.window.front();front->feedback.received_first_packet=std::numeric_limits<XrTime>::max()-1;CHECK(a.next_poll_deadline(std::numeric_limits<XrTime>::max())==a.next_nack_deadline(std::numeric_limits<XrTime>::max()));}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);auto*front=&a.window.front();front->feedback.received_first_packet=t+5;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s=std::make_shared<scenes::stream>();a.weak_scene=s;a.astc_deadline_enabled=true;add(a,0,0,t,false);add(a,0,2,t+1,true);CHECK(!a.window.has_newer_complete_than_front());auto due=a.next_poll_deadline(t+1);CHECK(due&&*due==t+1+nack_quiet_period_ns);}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s=std::make_shared<scenes::stream>();a.weak_scene=s;a.astc_deadline_enabled=true;add(a,0,0,t,false);add(a,0,2,t+1,true);CHECK(!a.window.has_newer_complete_than_front());auto due=a.next_poll_deadline(t+1);CHECK(due&&*due==t+1+nack_quiet_period_ns);}
 {const auto future=wivrn::nack_poll_timeout(10'000'000,10'000'001, std::chrono::milliseconds(100));CHECK(future.count()==1);const auto rounded=wivrn::nack_poll_timeout(10'000'000,12'500'001,std::chrono::milliseconds(100));CHECK(rounded.count()==3);CHECK(wivrn::nack_poll_timeout(12'500'001,12'500'001,std::chrono::milliseconds(100)).count()==0);CHECK(wivrn::nack_poll_timeout(10'000'000,{},std::chrono::milliseconds(100)).count()==100);}
}
int main(){std::puts("gate=actual extracted accumulator methods; virtual time; excludes constructor, push/process_packets, network thread, decoder/XR");test_retirement();test_controls();std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
'''
license=text[:text.index('#include "shard_accumulator.h"')]
(out/'quiet-retirement-gate.cpp').write_text(license+head+'\n\n'.join(methods)+foot)
provenance['files']={str(p.relative_to(repo)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [source,header,repo/'client/decoder/frame_window.h',repo/'client/decoder/shard_set.h',repo/'client/decoder/nack_deadline.h',network]}
provenance['process_packets_callsite']={'file':str(network.relative_to(repo)),'lines':[ntext[:ps].count('\n')+1,ntext[:pe+1].count('\n')+1],'sha256':hashlib.sha256(caller.encode()).hexdigest(),'structural_checks':['supplier uses next_poll_deadline','poll timeout is capped at 100ms','poll_nacks service follows network poll']}
(out/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
