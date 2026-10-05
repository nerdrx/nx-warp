#!/usr/bin/env python3
"""Generate a small typed-UDP arrival gate around the accepted kernel fixture."""
from pathlib import Path
import hashlib, json, sys

repo = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
src = repo / "client/decoder/shard_accumulator.cpp"
text = src.read_text()
sig = "void shard_accumulator::push_shard(video_stream_data_shard && shard)"
start = text.index(sig)
brace = text.index("{", start)
depth = 0
for end in range(brace, len(text)):
    if text[end] == "{":
        depth += 1
    elif text[end] == "}":
        depth -= 1
        if depth == 0:
            break
method = text[start:end + 1]
p = out / "kernel-gate.cpp"
s = p.read_text()
s = s.replace('#include <functional>', '#include <functional>\n#include <chrono>\n#include <cstdlib>', 1)
s = s.replace('namespace wivrn {', 'namespace wivrn {\nusing namespace wivrn::to_headset;', 1)
s = s.replace('window_t window{shard_set{2}};', 'window_t window{shard_set{0}};', 1)
s = s.replace('struct fake_instance { XrTime value=1; XrTime now()const{return value;} };',
'''struct fake_instance { XrTime value=1; bool realtime=false; XrTime now()const{return realtime?std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count():value;} };''', 1)
s = s.replace(' std::vector<uint64_t> completed; size_t bytes=0;\n void push_data(std::span<const std::span<const uint8_t>> payload,uint64_t,bool){for(auto p:payload)bytes+=p.size();}\n void frame_completed(const from_headset::feedback&fb,const to_headset::video_stream_data_shard::view_info_t&){completed.push_back(fb.frame_index);if(on_complete)on_complete();}',
''' std::vector<uint64_t> completed; std::vector<size_t> completed_bytes; uint64_t pending_frame=UINT64_MAX; size_t pending_bytes=0;
 void push_data(std::span<const std::span<const uint8_t>> payload,uint64_t frame,bool){if(frame!=pending_frame){pending_frame=frame;pending_bytes=0;}for(auto p:payload)pending_bytes+=p.size();}
 void frame_completed(const from_headset::feedback&fb,const to_headset::video_stream_data_shard::view_info_t&){completed.push_back(fb.frame_index);completed_bytes.push_back(pending_bytes);if(on_complete)on_complete();}''', 1)
s = s.replace(' void report_nacks(XrTime){} bool is_nxastc_codec() const{return nxastc_codec;}',
''' void report_nacks(XrTime){} bool is_nxastc_codec() const{return nxastc_codec;}
 void push_shard(video_stream_data_shard&&); void drain_parity(shard_set&){}''', 1)
anchor = 'static void debug_why_not_sent(const shard_accumulator::shard_set&){}'
assert anchor in s
s = s.replace(anchor, anchor + "\n" + method, 1)
s = s.replace('std::function<void()> stop_after_two;size_t calls=0;', 'std::function<void()> stop_after_two;size_t calls=0;size_t max_polls_before_stop=2;', 1)
s = s.replace('if(++calls==2&&stop_after_two)stop_after_two();', 'if(++calls==max_polls_before_stop&&stop_after_two)stop_after_two();', 1)
s = s.replace('std::array<item,1>decoders;', 'std::array<item,1>decoders;std::vector<std::pair<uint64_t,uint16_t>> arrivals;', 1)
s = s.replace('network_session->stop_after_two=[this]{state_=state::shutdown;};a->decoder_->on_complete=', 'network_session->stop_after_two=[this]{state_=state::shutdown;};a->decoder_->on_complete=', 1)
s = s.replace('std::array<item,1>decoders;std::vector<std::pair<uint64_t,uint16_t>> arrivals;', 'std::array<item,1>decoders;std::vector<std::pair<uint64_t,uint16_t>> arrivals;XrTime first_receipt=0;',1)
s = s.replace('template<class T>void operator()(T&&){}',
'''void operator()(to_headset::video_stream_data_shard&& shard){arrivals.emplace_back(shard.frame_idx,shard.shard_idx);if(shard.stream_item_idx==0&&decoders[0].decoder) {decoders[0].decoder->push_shard(std::move(shard));if(first_receipt==0&&decoders[0].decoder->window.front_index()==0)first_receipt=decoders[0].decoder->window.front().feedback.received_first_packet;}}
 template<class T>void operator()(T&&){}''', 1)
old_main = 'int main(){if(accumulator_gate_main())return 1;checks=0;failures=0;std::puts("kernel integration: exact process_packets + poll templates; host monotonic stand-in, no incoming packets");integrated_case(true,true,true);integrated_case(false,true,true);integrated_case(true,false,true);integrated_case(true,true,false);std::printf("checks=%d failures=%d\\n",checks,failures);return failures?1:0;}'
assert old_main in s
test = r'''
using data_shard=wivrn::to_headset::video_stream_data_shard;
static std::array<std::array<std::array<uint8_t,5>,3>,2> wire_bytes{};
static data_shard make_wire_shard(uint64_t frame,uint16_t index,bool terminal=false){data_shard d{};d.stream_item_idx=0;d.frame_idx=frame;d.shard_idx=index;auto&b=wire_bytes[frame][index];b.fill(uint8_t(0x20+frame*4+index));d.payload=b;if(index==0)d.view_info=data_shard::view_info_t{.display_time=42};if(terminal)d.timing_info=data_shard::timing_info_t{1,2,3,4};return d;}
static void send_initial(caller_projection::scenes::stream&loop,bool timely_repair=false){auto&peer=loop.network_session->udp_peer;peer.send(make_wire_shard(0,0));peer.send(make_wire_shard(0,2,true));peer.send(make_wire_shard(1,0));peer.send(make_wire_shard(1,1,true));if(timely_repair)peer.send(make_wire_shard(0,1));}
static void test_real_udp_arrivals(){
 checks=0;failures=0;std::puts("typed UDP -> exact push_shard -> quiet deadline service; steady_clock receipt times; fake XR/decoder/scene; direct timely-repair injection");
 auto a=std::make_shared<shard_accumulator>();a->window=shard_accumulator::window_t{shard_set{0}};a->instance.realtime=true;a->astc_deadline_enabled=true;auto scene=std::make_shared<wivrn::scenes::stream>();scene->period=11'111'111;a->weak_scene=scene;caller_projection::enabled=true;caller_projection::scenes::stream loop(a);loop.network_session->max_polls_before_stop=10;send_initial(loop);auto began=host_ns();loop.process_packets();auto ended=host_ns();auto&sock=*loop.network_session;CHECK(sock.bytes_received_.load()>0);CHECK(loop.arrivals.size()==4);CHECK(loop.arrivals[0]==std::pair<uint64_t,uint16_t>{0,0});CHECK(loop.arrivals[1]==std::pair<uint64_t,uint16_t>{0,2});CHECK(loop.arrivals[2]==std::pair<uint64_t,uint16_t>{1,0});CHECK(loop.arrivals[3]==std::pair<uint64_t,uint16_t>{1,1});auto first=loop.first_receipt;CHECK(first>=began&&first<=ended);CHECK(a->feedback_frames==std::vector<uint64_t>({0,1}));CHECK(a->decoder_->completed==std::vector<uint64_t>{1});CHECK(a->decoder_->completed_bytes==std::vector<size_t>{10});CHECK(scene->requests.size()==2);CHECK(a->window.front_index()==2);CHECK(loop.state_==caller_projection::scenes::stream::state::shutdown);std::printf("quiet bytes=%llu first_ns=%lld polls=%zu nack_requests=%zu feedback_count=%zu decoded=%llu payload_bytes=%zu\n",(unsigned long long)sock.bytes_received_.load(),(long long)first,sock.calls,scene->requests.size(),a->feedback_frames.size(),(unsigned long long)a->decoder_->completed.at(0),a->decoder_->completed_bytes.at(0));
 // A late shard travels through the same UDP receiver and exact push_shard; its old index is ignored.
 sock.udp_peer.send(make_wire_shard(0,1));sock.poll(loop,10ms,[&]{return 10ms;});CHECK(a->window.front_index()==2);CHECK(a->decoder_->completed==std::vector<uint64_t>{1});
 // With deadline retirement disabled, real arrivals do not release the newer complete frame during silence.
 auto b=std::make_shared<shard_accumulator>();b->window=shard_accumulator::window_t{shard_set{0}};b->instance.realtime=true;b->astc_deadline_enabled=false;auto sc=std::make_shared<wivrn::scenes::stream>();sc->period=11'111'111;b->weak_scene=sc;caller_projection::scenes::stream control(b);control.network_session->max_polls_before_stop=4;send_initial(control);auto control_start=host_ns();control.process_packets();CHECK(host_ns()-control_start>=2*sc->period);CHECK(control.network_session->bytes_received_.load()>0);CHECK(b->window.front_index()==0);CHECK(b->decoder_->completed.empty());
 // A missing interior shard arriving before retirement completes and delivers both frames in order.
 auto c=std::make_shared<shard_accumulator>();c->window=shard_accumulator::window_t{shard_set{0}};c->instance.realtime=true;c->astc_deadline_enabled=true;auto cs=std::make_shared<wivrn::scenes::stream>();cs->period=11'111'111;c->weak_scene=cs;caller_projection::scenes::stream timely(c);timely.network_session->max_polls_before_stop=10;send_initial(timely,true);timely.process_packets();CHECK(timely.network_session->bytes_received_.load()>0);CHECK(c->decoder_->completed==std::vector<uint64_t>({0,1}));CHECK(c->feedback_frames==std::vector<uint64_t>({0,1}));CHECK(c->decoder_->completed_bytes==std::vector<size_t>({15,10}));
 std::printf("checks=%d failures=%d\\n",checks,failures);if(failures)std::exit(1);
}
'''
s = s.replace(old_main, test + '\nint main(){test_real_udp_arrivals();return 0;}')
for frame in ('a', 'b', 'c'):
    s = s.replace(f'{frame}->window=shard_accumulator::window_t{{shard_set{{0}}}};', '', 1)
s = s.replace('CHECK(loop.arrivals[0]==std::pair<uint64_t,uint16_t>{0,0});CHECK(loop.arrivals[1]==std::pair<uint64_t,uint16_t>{0,2});CHECK(loop.arrivals[2]==std::pair<uint64_t,uint16_t>{1,0});CHECK(loop.arrivals[3]==std::pair<uint64_t,uint16_t>{1,1});',
'''CHECK(loop.arrivals[0].first==0&&loop.arrivals[0].second==0);CHECK(loop.arrivals[1].first==0&&loop.arrivals[1].second==2);CHECK(loop.arrivals[2].first==1&&loop.arrivals[2].second==0);CHECK(loop.arrivals[3].first==1&&loop.arrivals[3].second==1);''', 1)
s=s.replace('std::vector<size_t> completed_bytes;', 'std::vector<std::vector<uint8_t>> completed_payloads; std::vector<uint8_t> pending_payload; std::vector<size_t> completed_bytes;',1)
s=s.replace('pending_frame=frame;pending_bytes=0;', 'pending_frame=frame;pending_bytes=0;pending_payload.clear();',1)
s=s.replace('for(auto p:payload)pending_bytes+=p.size();', 'for(auto p:payload){pending_bytes+=p.size();pending_payload.insert(pending_payload.end(),p.begin(),p.end());}',1)
s=s.replace('completed_bytes.push_back(pending_bytes);', 'completed_bytes.push_back(pending_bytes);completed_payloads.push_back(pending_payload);',1)
s=s.replace('CHECK(a->decoder_->completed_bytes==std::vector<size_t>{10});', 'CHECK(a->decoder_->completed_bytes==std::vector<size_t>{10});CHECK(a->decoder_->completed_payloads.size()==1&&a->decoder_->completed_payloads[0]==std::vector<uint8_t>({36,36,36,36,36,37,37,37,37,37}));',1)
s=s.replace('CHECK(c->decoder_->completed_bytes==std::vector<size_t>({15,10}));', 'CHECK(c->decoder_->completed_bytes==std::vector<size_t>({15,10}));CHECK(c->decoder_->completed_payloads.size()==2&&c->decoder_->completed_payloads[0]==std::vector<uint8_t>({32,32,32,32,32,33,33,33,33,33,34,34,34,34,34}));CHECK(c->decoder_->completed_payloads[1]==a->decoder_->completed_payloads[0]);',1)
s=s.replace('std::printf("checks=%d failures=%d\\n",checks,failures);if(failures)', 'std::printf("checks=%d failures=%d\n",checks,failures);if(failures)',1)
s=s.replace('sock.udp_peer.send(make_wire_shard(0,1));sock.poll(loop,10ms,[&]{return 10ms;});', 'auto late_bytes=sock.bytes_received_.load();sock.udp_peer.send(make_wire_shard(0,1));sock.poll(loop,10ms,[&]{return 10ms;});CHECK(sock.bytes_received_.load()>late_bytes);CHECK(loop.arrivals.size()==5&&loop.arrivals.back().first==0&&loop.arrivals.back().second==1);',1)
s=s.replace('CHECK(b->window.front_index()==0);CHECK(b->decoder_->completed.empty());', 'CHECK(b->window.front_index()==0);CHECK(b->decoder_->completed.empty());CHECK(control.arrivals.size()==4);',1)
s=s.replace(' std::printf("checks=%d failures=%d\\\\n",checks,failures);if(failures)', ' std::printf("case,quiet,%llu,%zu,%zu\\n",(unsigned long long)sock.bytes_received_.load(),a->decoder_->completed.size(),sock.calls);std::printf("case,deadline_off,%llu,%zu,%zu\\n",(unsigned long long)control.network_session->bytes_received_.load(),b->decoder_->completed.size(),control.network_session->calls);std::printf("case,timely_direct_repair,%llu,%zu,%zu\\n",(unsigned long long)timely.network_session->bytes_received_.load(),c->decoder_->completed.size(),timely.network_session->calls);std::printf("checks=%d failures=%d\\n",checks,failures);if(failures)',1)
p.write_text(s)
prov = json.loads((out / "provenance.json").read_text())
prov["exact_push_shard"] = {"source_sha256": hashlib.sha256(method.encode()).hexdigest(), "source_lines": [text[:start].count("\n")+1, text[:end+1].count("\n")+1], "scope": "exact production method; drain_parity stubbed because fixture carries data shards only"}
prov["generated_cpp_sha256"]=hashlib.sha256(s.encode()).hexdigest()
prov["scope"]="Host typed UDP arrivals and exact push/poll/service bodies; accumulator constructor, parity drain, config, scene, XR and decoder are substitutes."
prov["fixture"] = "typed IPv6 localhost UDP data shards -> exact process_packets poll visitor -> exact extracted push_shard; steady_clock now() stand-in; fake XR, decoder, scene, parity drain; no Android/network server"
(out / "provenance.json").write_text(json.dumps(prov, indent=2) + "\n")
