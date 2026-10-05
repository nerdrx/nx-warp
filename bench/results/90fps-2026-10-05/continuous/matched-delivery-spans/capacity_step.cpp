#include "driver/bitrate_controller.h"
#include "encoder/shard_pacer.h"
#include "util/u_logging.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

extern "C" void u_log(const char *, int, const char *, enum u_logging_level, const char *, ...) {}
extern "C" enum u_logging_level u_log_get_global_level(void) { return U_LOGGING_INFO; }
using C = wivrn::bitrate_controller;
using tp = C::clock::time_point;
using state = wivrn::to_headset::transport_status::controller_state;
constexpr int64_t period=11'111'111;
constexpr uint64_t late_begin=25'000'000'000ull, end_ns=40'000'000'000ull, B=8'000'000'000ull;
const char *name(state s){using E=state;switch(s){case E::off:return"off";case E::startup:return"startup";case E::steady:return"steady";case E::probe:return"probe";case E::recovering:return"recovering";}return"?";}
struct result{uint32_t min_target=0,final_target=0,estimate=0;size_t probe_before=0,probe_after=0,downs_after=0,late_feedback=0;uint64_t first_down_ns=0;uint32_t first_down_from=0,first_down_to=0;int64_t max_lag=0,end=0;};

bool run(const std::string &label,uint64_t step_ns,uint64_t cap_after,bool late_only,const std::string &path,result &r){
 C ctl;ctl.configure({.enabled=true},1'000'000'000,true,false,C::mode::bbr);ctl.set_pacing_window(.4f);wivrn::pacing_slot slot;
 FILE*f=std::fopen(path.c_str(),"w");if(!f)return false;
 std::fprintf(f,"label,frame,desired_ns,start_ns,capacity_bps,late_feedback,bitrate_bps,eye0_bytes,eye0_span_ns,eye1_bytes,eye1_span_ns,serial_ns,lag_ns,estimate_bps,state,probe_entry,down_event,previous_state\n");
 int64_t prev_end=0;uint32_t prev_target=ctl.current();r.min_target=prev_target;
 for(uint64_t frame=0;frame<uint64_t(end_ns/period);++frame){
  const uint64_t desired=frame*uint64_t(period);const int64_t start=std::max<int64_t>(int64_t(desired),prev_end);
  const uint64_t capacity=uint64_t(start)<step_ns?500'000'000ull:cap_after;
  const bool late=late_only && uint64_t(start)>=late_begin;
  const uint32_t target=ctl.current();const uint64_t total=uint64_t(double(target)*double(period)/8e9);
  const uint32_t bytes[2]={uint32_t(total/2),uint32_t(total-total/2)};int64_t budgets[2]={};
  auto span=[&](uint32_t b,int64_t t,size_t queued,int64_t&budget){budget=slot.begin_frame(t,period,.4f,queued);const bool active=wivrn::shard_pacer(t,budget,b).active();const uint64_t serial=(uint64_t(b)*B+capacity-1)/capacity;return int64_t(std::max<uint64_t>(active?uint64_t(budget):0,serial));};
  const int64_t eye0=span(bytes[0],start,1,budgets[0]);const int64_t eye1_start=start+eye0;const int64_t eye1=span(bytes[1],eye1_start,0,budgets[1]);const int64_t ends[2]={start+eye0,eye1_start+eye1};const int64_t now_ns=ends[1];const tp now=tp{}+std::chrono::hours(1)+std::chrono::nanoseconds(now_ns);const XrTime base=1'000'000'000LL+XrTime(start);
  bool probe_entry=false,down_event=false;uint32_t down_from=0,down_to=0;const state before_state=ctl.snapshot().state;state old_state=before_state;
  for(uint8_t eye=0;eye<2;++eye){
   const int64_t first=eye?eye1_start:start,last=ends[eye];ctl.on_frame_bytes(frame,eye,bytes[eye],now);
   wivrn::from_headset::feedback fb{};fb.frame_index=frame;fb.stream_index=eye;fb.received_first_packet=base+XrTime(first-start);fb.received_last_packet=base+XrTime(last-start);fb.sent_to_decoder=fb.received_last_packet+1'000'000;fb.send_begin=fb.received_first_packet+2'000'000'000LL;fb.send_end=fb.received_last_packet+2'000'000'000LL;
   if(late){fb.received_from_decoder=fb.sent_to_decoder+1'000'000;fb.blitted=0;fb.times_displayed=0;++r.late_feedback;}
   else{fb.received_from_decoder=fb.sent_to_decoder+1'000'000;fb.blitted=fb.received_from_decoder+1'000'000;fb.times_displayed=1;}
   ctl.on_feedback(fb,period,true,now);const auto snap=ctl.snapshot();
   if(snap.state==state::probe && old_state!=state::probe)probe_entry=true;
   if(snap.bitrate_bps<prev_target){down_event=true;down_from=prev_target;down_to=snap.bitrate_bps;}
   prev_target=snap.bitrate_bps;
   old_state=snap.state;
  }
  prev_end=ends[1];const auto snap=ctl.snapshot();r.min_target=std::min(r.min_target,snap.bitrate_bps);r.max_lag=std::max(r.max_lag,start-int64_t(desired));
  if(probe_entry){if(uint64_t(start)<step_ns)++r.probe_before;else ++r.probe_after;}
  if(down_event && uint64_t(start)>=step_ns){++r.downs_after;if(!r.first_down_ns){r.first_down_ns=uint64_t(start);r.first_down_from=down_from;r.first_down_to=down_to;}}
  const uint64_t serial=uint64_t(ends[1]-start);std::fprintf(f,"%s,%llu,%llu,%lld,%llu,%d,%u,%u,%lld,%u,%lld,%llu,%lld,%u,%s,%d,%d,%s\n",label.c_str(),(unsigned long long)frame,(unsigned long long)desired,(long long)start,(unsigned long long)capacity,late?1:0,target,bytes[0],(long long)eye0,bytes[1],(long long)eye1,(unsigned long long)serial,(long long)(start-int64_t(desired)),ctl.bandwidth_estimate(),name(snap.state),probe_entry?1:0,down_event?1:0,name(before_state));
 }
 if(std::fclose(f)!=0)return false;r.final_target=ctl.current();r.estimate=ctl.bandwidth_estimate();r.end=prev_end;
 std::printf("%s cap=%llu at=%llu late=%d final=%u min=%u estimate=%u probes_pre/post=%zu/%zu downs_after=%zu first_down=%llu:%u->%u latefb=%zu lag=%lld end=%lld\n",label.c_str(),(unsigned long long)cap_after,(unsigned long long)step_ns,late_only?1:0,r.final_target,r.min_target,r.estimate,r.probe_before,r.probe_after,r.downs_after,(unsigned long long)r.first_down_ns,r.first_down_from,r.first_down_to,r.late_feedback,(long long)r.max_lag,(long long)r.end);
 return true;
}

int main(int argc,char**argv){if(argc!=2)return 2;const std::string dir=argv[1];
 struct S{const char*name;uint64_t at,cap;bool late;};
 const S scenarios[]={{"rise3-clean",3'000'000'000ull,1'000'000'000ull,false},{"rise3-late",3'000'000'000ull,1'000'000'000ull,true},{"rise5-clean",5'000'000'000ull,1'000'000'000ull,false},{"rise5-late",5'000'000'000ull,1'000'000'000ull,true},{"rise10-clean",10'000'000'000ull,1'000'000'000ull,false},{"rise10-late",10'000'000'000ull,1'000'000'000ull,true},{"rise20-clean",20'000'000'000ull,1'000'000'000ull,false},{"rise20-late",20'000'000'000ull,1'000'000'000ull,true},{"fall300-clean",10'000'000'000ull,300'000'000ull,false},{"fall250-clean",10'000'000'000ull,250'000'000ull,false}};
 for(const auto&s:scenarios){result r{};if(!run(s.name,s.at,s.cap,s.late,dir+"/"+s.name+".csv",r))return 2;}
}
