#!/usr/bin/env python3
import hashlib, json, pathlib, re, sys
repo=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2]); out.mkdir(parents=True,exist_ok=True)
source=repo/'client/decoder/shard_accumulator.cpp'; text=source.read_text()
header=repo/'client/decoder/shard_accumulator.h'; htext=header.read_text()
signatures=['void shard_accumulator::pump(XrTime now)', 'shard_accumulator::window_t::step shard_accumulator::try_submit_front(shard_set & current)', 'void shard_accumulator::poll_nacks(XrTime now)', 'void shard_accumulator::try_nack(XrTime now)']
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
namespace scenes { struct stream { std::vector<from_headset::nack> requests; void send_nack(const from_headset::nack&n){requests.push_back(n);} XrDuration display_period_ns()const{return 11'111'111;} }; }
struct fake_instance { XrTime value=1; XrTime now()const{return value;} };
struct fake_decoder {
 std::vector<uint64_t> completed;
 size_t bytes=0;
 void push_data(std::span<const std::span<const uint8_t>> payload,uint64_t,bool){for(auto p:payload)bytes+=p.size();}
 void frame_completed(const from_headset::feedback&fb,const to_headset::video_stream_data_shard::view_info_t&){completed.push_back(fb.frame_index);}
};
class shard_accumulator {
public:
 using shard_set=wivrn::shard_set;
 using data_shard=to_headset::video_stream_data_shard;
 __ALIAS__
 window_t window{shard_set{2}};
 fake_instance instance;
 std::shared_ptr<fake_decoder> decoder_=std::make_shared<fake_decoder>();
 std::weak_ptr<scenes::stream> weak_scene;
 bool astc_deadline_enabled=false;
 bool nxastc_codec=true;
 std::vector<uint16_t> nack_scratch;
 uint64_t nack_requests=0,nack_shards=0;
 std::atomic<uint64_t> nack_shards_total=0;
 void report_nacks(XrTime){}
 void poll_nacks(XrTime);
 void try_nack(XrTime);
 std::vector<uint64_t> feedback_frames;
 void send_feedback(from_headset::feedback&fb){feedback_frames.push_back(fb.frame_index);}
 void pump(XrTime);
 window_t::step try_submit_front(shard_set&);
};
static void debug_why_not_sent(const shard_accumulator::shard_set&){}
'''.replace('__ALIAS__',alias)
foot=r'''
}
using namespace wivrn;
static int checks=0, failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::printf("FAIL:%d %s\n",__LINE__,#x);}}while(false)
static void add(shard_accumulator&a,uint64_t frame,uint16_t index,XrTime now,bool tail){
 auto*s=a.window.slot(frame,[](auto&){});CHECK(s);to_headset::video_stream_data_shard p{};
 p.stream_item_idx=2;p.frame_idx=frame;p.shard_idx=index;
 if(index==0)p.view_info=to_headset::video_stream_data_shard::view_info_t{};
 if(tail)p.timing_info=to_headset::video_stream_data_shard::timing_info_t{1,2,3,4};
 static std::array<uint8_t,4> bytes{1,2,3,4};p.payload=bytes;s->insert(std::move(p),now);
 if(s->complete())a.window.note_complete(frame);
 a.instance.value=now;a.pump(now);
}
static void test(bool enabled,bool newer){
 shard_accumulator a;auto scene=std::make_shared<scenes::stream>();a.weak_scene=scene;a.astc_deadline_enabled=enabled;
 // Frame0 has a confirmed interior hole1; its tail2 is known. Frame1 is complete.
 const XrTime first=1'000'000;add(a,0,0,first,false);add(a,0,2,first+1,true);
 if(newer){add(a,1,0,first+100,false);add(a,1,1,first+101,true);}
 CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());
 const XrTime due=first+2*scene->display_period_ns();
 a.instance.value=due-1;a.pump(due-1);CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());
 // Actual quiet polling entry spends both NACK rounds, but does not pump.
 a.instance.value=due;a.poll_nacks(due);
 a.instance.value=due+nack_quiet_period_ns;a.poll_nacks(a.instance.value);
 a.instance.value=due+100'000'000;a.poll_nacks(a.instance.value);
 CHECK(scene->requests.size()==2);CHECK(a.window.front().nack_rounds==2);
 CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());
 a.pump(a.instance.value);
 const bool release=enabled&&newer;
 CHECK(a.window.front_index()==(release?2u:0u));CHECK(a.decoder_->completed.size()==(release?1u:0u));
 if(release){CHECK(a.decoder_->completed[0]==1);CHECK(a.feedback_frames.size()==2);}
 std::printf("%d,%d,%lld,%llu,%zu\n",enabled,newer,(long long)due,(unsigned long long)a.window.front_index(),a.decoder_->completed.size());
}
int main(){std::puts("deadline_enabled,newer_complete,virtual_due_ns,front_after_explicit_pump,decoder_stub_completed");test(true,true);test(false,true);test(true,false);std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
'''
license=text[:text.index('#include "shard_accumulator.h"')]
(out/'pump-gate.cpp').write_text(license+head+'\n\n'.join(methods)+foot)
provenance['files']={str(p.relative_to(repo)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [source,header,repo/'client/decoder/frame_window.h',repo/'client/decoder/shard_set.h']}
(out/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
