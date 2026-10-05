#!/usr/bin/env python3
"""Join actual typed NACK/retained-history reply to the published arrival gate."""
import hashlib,json,sys
from pathlib import Path
repo=Path(sys.argv[1]);out=Path(sys.argv[2]);cpp=out/'kernel-gate.cpp';s=cpp.read_text()
s=s.replace('#include <functional>','#include <functional>\n#include "shard_history.h"\n#include "fec.h"',1)
old='void send_nack(const from_headset::nack&n){requests.push_back(n);}'
assert s.count(old)==1
s=s.replace(old,'std::function<void(const from_headset::nack&)> wire_send; void send_nack(const from_headset::nack&n){requests.push_back(n);if(wire_send)wire_send(n);}',1)
old='wivrn::UDP tx;tx.connect(in6addr_loopback,ntohs(actual.sin6_port));'
assert s.count(old)==1
s=s.replace(old,old+'sockaddr_in6 peeraddr{};n=sizeof(peeraddr);if(getsockname(tx.get_fd(),(sockaddr*)&peeraddr,&n)!=0)throw std::runtime_error("UDP peer address");rx.connect(in6addr_loopback,ntohs(peeraddr.sin6_port));',1)
s=s.replace('std::function<void()> stop_after_two;','std::function<void()> peer_service;std::function<void()> stop_after_two;',1)
s=s.replace('int ret=poll_projection::poll(', 'if(peer_service)peer_service();int ret=poll_projection::poll(',1)
s=s.replace('std::vector<uint64_t> feedback_frames;','std::vector<uint64_t> feedback_frames;std::vector<from_headset::feedback> feedback_records;',1)
s=s.replace('feedback_frames.push_back(fb.frame_index);','feedback_frames.push_back(fb.frame_index);feedback_records.push_back(fb);',1)
main='int main(){test_real_udp_arrivals();return 0;}'
assert s.count(main)==1
new=r"""
static void wire_case(int mode){
 auto a=std::make_shared<shard_accumulator>();a->instance.realtime=true;a->astc_deadline_enabled=true;
 auto scene=std::make_shared<wivrn::scenes::stream>();scene->period=11'111'111;a->weak_scene=scene;
 caller_projection::enabled=true;caller_projection::scenes::stream loop(a);auto&session=*loop.network_session;session.max_polls_before_stop=10;
 shard_history history;history.set_enabled(true);
 std::array<data_shard,3> old{make_wire_shard(0,0),make_wire_shard(0,1),make_wire_shard(0,2,true)};
 for(auto&packet:old){std::vector<uint8_t>blob;fec::encode_blob(packet,blob);if(mode!=2)history.push(0,packet.shard_idx,blob,true);}history.end_frame(0,3);
 size_t peer_requests=0,history_hits=0,replies=0;std::vector<from_headset::nack>wire_requests;
 scene->wire_send=[&](const from_headset::nack&n){session.stream.send(from_headset::nack(n));};
 session.peer_service=[&]{
  pollfd ready{.fd=session.udp_peer.get_fd(),.events=POLLIN};
  if(::poll(&ready,1,0)==0)return;
  auto received=session.udp_peer.receive();CHECK(received.has_value());if(!received)return;
  CHECK(std::holds_alternative<from_headset::nack>(*received));if(!std::holds_alternative<from_headset::nack>(*received))return;
  auto request=std::get<from_headset::nack>(std::move(*received));++peer_requests;wire_requests.push_back(request);
  CHECK(request.stream_index==0&&request.frame_idx==0&&request.first_shard_idx==1);
  CHECK(request.bitmap==std::vector<uint8_t>{1});
  std::vector<shard_history::hit>hits;auto count=history.collect(request.frame_idx,request.first_shard_idx,request.bitmap,64,hits);
  CHECK(count==hits.size());CHECK(hits.size()==(mode==2?0u:1u));history_hits+=hits.size();
  for(auto&hit:hits){
   CHECK(hit.shard_idx==1);auto data=fec::decode_blob(0,0,hit.shard_idx,hit.blob);
   CHECK(data.payload.size()==5&&std::equal(data.payload.begin(),data.payload.end(),old[1].payload.begin()));
   CHECK(!data.view_info&&!data.timing_info);
   if(mode==0){session.udp_peer.send(std::move(data));++replies;}
  }
 };
 send_initial(loop);auto start=host_ns();loop.process_packets();auto finish=host_ns();
 CHECK(session.bytes_received_.load()>0);CHECK(loop.first_receipt>=start&&loop.first_receipt<=finish);
 CHECK(a->window.front_index()==2);CHECK(a->feedback_records.size()==2);
 CHECK(peer_requests==scene->requests.size());CHECK(peer_requests==(mode==0?1u:2u));
 CHECK(replies==(mode==0?1u:0u));CHECK(history_hits==(mode==2?0u:peer_requests));
 if(mode==0){
  CHECK(loop.arrivals.size()==5);CHECK(loop.arrivals.back().first==0&&loop.arrivals.back().second==1);
  CHECK(a->decoder_->completed==std::vector<uint64_t>({0,1}));
  CHECK(a->decoder_->completed_payloads.size()==2);
  CHECK(a->decoder_->completed_payloads[0]==std::vector<uint8_t>({32,32,32,32,32,33,33,33,33,33,34,34,34,34,34}));
  CHECK(a->decoder_->completed_payloads[1]==std::vector<uint8_t>({36,36,36,36,36,37,37,37,37,37}));
  CHECK(std::all_of(a->feedback_records.begin(),a->feedback_records.end(),[](const auto&f){return f.sent_to_decoder>0;}));
 }else{
  CHECK(loop.arrivals.size()==4);CHECK(a->decoder_->completed==std::vector<uint64_t>{1});
  CHECK(a->decoder_->completed_payloads.size()==1&&a->decoder_->completed_payloads[0]==std::vector<uint8_t>({36,36,36,36,36,37,37,37,37,37}));
  CHECK(a->feedback_records[0].frame_index==0&&a->feedback_records[0].sent_to_decoder==0);
  CHECK(a->feedback_records[1].frame_index==1&&a->feedback_records[1].sent_to_decoder>0);
 }
 size_t retired=std::count_if(a->feedback_records.begin(),a->feedback_records.end(),[](const auto&f){return f.sent_to_decoder==0;});
 std::printf("case,%d,received_bytes=%llu,nacks=%zu,hits=%zu,replies=%zu,retired=%zu,completed=%zu\n",mode,(unsigned long long)session.bytes_received_.load(),peer_requests,history_hits,replies,retired,a->decoder_->completed.size());
 scene->wire_send={};session.peer_service={};
}
int main(){checks=0;failures=0;std::puts("actual UDP NACK/history reply joined with exact arrival/service methods; substituted scene/XR/decoder; synchronous primary peer");wire_case(0);wire_case(1);wire_case(2);std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
"""
s=s.replace(main,new);cpp.write_text(s)
meta=json.loads((out/'provenance.json').read_text());meta['generated_cpp_sha256']=hashlib.sha256(s.encode()).hexdigest();meta['wire_join_scope']='Typed UDP reverse NACK receive, actual enabled-primary history collect/FEC blob decode, typed data reply into exact push_shard. Synchronous peer before next poll; not actual server handler or radio.'
for rel in ['server/encoder/shard_history.h','common/fec.h','common/wivrn_packets.h','common/wivrn_sockets.cpp','common/wivrn_sockets.h']:
 meta.setdefault('wire_dependency_hashes',{})[rel]=hashlib.sha256((repo/rel).read_bytes()).hexdigest()
(out/'provenance.json').write_text(json.dumps(meta,indent=2)+'\n')
