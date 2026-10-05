#include "driver/bitrate_controller.h"
#include "encoder/shard_pacer.h"
#include "util/u_logging.h"
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <string>

extern "C" void u_log(const char *, int, const char *, enum u_logging_level, const char *format, ...) {
  if (!std::strstr(format, "Automatic bitrate v2: %s,")) return;
  va_list args, peek;
  va_start(args, format); va_copy(peek, args);
  const char *reason = va_arg(peek, const char *);
  va_end(peek);
  if (std::strcmp(reason,"backing off") == 0) { std::vprintf(format,args); std::printf("\n"); }
  va_end(args);
}
extern "C" enum u_logging_level u_log_get_global_level(void) { return U_LOGGING_INFO; }
using ctl_t = wivrn::bitrate_controller;
using tp = ctl_t::clock::time_point;
constexpr int64_t period_ns = 11'111'111;
constexpr float pacing_window = 0.4f;
constexpr uint64_t bps_to_ns = 8'000'000'000ull;
constexpr uint64_t phase_late_start = 6'000'000'000ull;
constexpr uint64_t phase_late_end = 20'000'000'000ull;
constexpr uint64_t recovery_end = 40'000'000'000ull;
using controller_state = wivrn::to_headset::transport_status::controller_state;

const char *state_name(controller_state s) {
  using E = controller_state;
  switch (s) { case E::off:return "off"; case E::steady:return "steady"; case E::startup:return "startup"; case E::probe:return "probe"; case E::recovering:return "recovering"; }
  return "?";
}
struct result { uint32_t final_target=0; size_t probes_late=0, probes_recovery=0, probes_clean=0, late_inputs=0; int64_t end_ns=0; };

bool run(const std::string &label, const std::string &feedback_kind, const std::string &path, result &out) {
  std::printf("TRACE %s/%s\n",label.c_str(),feedback_kind.c_str());
  ctl_t ctl;
  ctl.configure({.enabled=true}, 1'000'000'000, true, false, ctl_t::mode::bbr);
  ctl.set_pacing_window(pacing_window);
  wivrn::pacing_slot slot;
  std::FILE *f=std::fopen(path.c_str(),"w"); if(!f)return false;
  std::fprintf(f,"label,frame,desired_ns,capacity_bps,phase,feedback_kind,bitrate_bps,eye0_bytes,eye0_span_ns,eye1_bytes,eye1_span_ns,serial_ns,lag_ns,estimate_bps,state,probe_entry\n");
  int64_t prev_end=0; auto prev_state=ctl.snapshot().state;
  size_t probe_late=0,probe_recovery=0,probe_clean=0,late_inputs=0;
  for(uint64_t frame=0; frame<uint64_t(recovery_end/period_ns); ++frame) {
    const uint64_t desired=frame*uint64_t(period_ns);
    const uint64_t capacity=desired < phase_late_end ? 500'000'000ull : 1'000'000'000ull;
    const bool in_late=desired>=phase_late_start && desired<phase_late_end &&
      (feedback_kind=="late" || feedback_kind=="late-one-eye");
    const bool early_only=desired>=phase_late_start && desired<phase_late_end && feedback_kind=="early";
    const int64_t start=std::max<int64_t>(int64_t(desired),prev_end);
    const uint32_t target=ctl.current();
    // Model the aggregate stream output at its requested target, split equally by eyes.
    const uint64_t total=uint64_t(double(target)*double(period_ns)/8e9);
    const uint32_t bytes[2]={uint32_t(total/2),uint32_t(total-total/2)};
    int64_t budgets[2]={};
    auto span_for=[&](uint32_t b,int64_t t,size_t queued,int64_t &budget){
      budget=slot.begin_frame(t,period_ns,pacing_window,queued);
      const bool active=wivrn::shard_pacer(t,budget,b).active();
      const uint64_t serial=(uint64_t(b)*bps_to_ns+capacity-1)/capacity;
      return int64_t(std::max<uint64_t>(active?uint64_t(budget):0,serial));
    };
    const int64_t spans[2]={span_for(bytes[0],start,1,budgets[0]),0};
    const int64_t eye1_start=start+spans[0];
    const int64_t eye1_span=span_for(bytes[1],eye1_start,0,budgets[1]);
    const int64_t ends[2]={start+spans[0],eye1_start+eye1_span};
    const int64_t now_ns=ends[1];
    const tp now=tp{}+std::chrono::hours(1)+std::chrono::nanoseconds(now_ns);
    const XrTime base=1'000'000'000LL+XrTime(start);
    bool probe_entry=false;
    for(uint8_t eye=0;eye<2;++eye){
      const int64_t first=eye?eye1_start:start, last=ends[eye];
      ctl.on_frame_bytes(frame,eye,bytes[eye],now);
      wivrn::from_headset::feedback fb{};
      fb.frame_index=frame; fb.stream_index=eye;
      fb.received_first_packet=base+XrTime(first-start);
      fb.received_last_packet=base+XrTime(last-start);
      fb.sent_to_decoder=fb.received_last_packet+1'000'000; // complete/reassembled, no packet loss
      if(in_late && (feedback_kind!="late-one-eye" || eye==1)){
        // Models received_from_decoder feedback for a decoded/accepted image later dropped before presentation.
        fb.received_from_decoder=fb.sent_to_decoder+1'000'000;
        fb.blitted=0; fb.times_displayed=0; ++late_inputs;
      } else if(!early_only){
        fb.received_from_decoder=fb.sent_to_decoder+1'000'000;
        fb.blitted=fb.received_from_decoder+1'000'000; fb.times_displayed=1;
      } // early-only: handed to decoder, but no received_from_decoder/presentation result yet
      ctl.on_feedback(fb,period_ns,true,now);
      const auto state=ctl.snapshot().state;
      if(state==controller_state::probe && prev_state!=controller_state::probe){
        probe_entry=true;
        if(desired>=phase_late_start && desired<phase_late_end) ++probe_late;
        else if(desired>=phase_late_end) ++probe_recovery;
        else ++probe_clean;
      }
      prev_state=state;
    }
    prev_end=ends[1];
    const auto current=ctl.snapshot();
    const uint64_t serial_ns=uint64_t(ends[1]-start);
    const char *phase=desired<phase_late_start?"warmup":desired<phase_late_end?"late_window":"recovery";
    std::fprintf(f,"%s,%llu,%llu,%llu,%s,%s,%u,%u,%lld,%u,%lld,%llu,%lld,%u,%s,%d\n",label.c_str(),
      (unsigned long long)frame,(unsigned long long)desired,(unsigned long long)capacity,phase,feedback_kind.c_str(),target,
      bytes[0],(long long)spans[0],bytes[1],(long long)eye1_span,(unsigned long long)serial_ns,(long long)(start-int64_t(desired)),
      ctl.bandwidth_estimate(),state_name(current.state),probe_entry?1:0);
  }
  if (std::fclose(f) != 0) return false;
  out={ctl.current(),probe_late,probe_recovery,probe_clean,late_inputs,prev_end};
  std::printf("%s/%s final=%u probes_late=%zu probes_recovery=%zu probes_clean=%zu late_fb=%zu end=%lld\n",label.c_str(),feedback_kind.c_str(),out.final_target,out.probes_late,out.probes_recovery,out.probes_clean,out.late_inputs,(long long)out.end_ns);
  return true;
}
int main(int argc,char **argv){
  if(argc!=3)return 2;
  const std::string label=argv[1], dir=argv[2]; result clean{},late{},late_one{},early{};
  if(!run(label,"clean",dir+"/"+label+"-clean.csv",clean) ||
     !run(label,"late",dir+"/"+label+"-late.csv",late) ||
     !run(label,"late-one-eye",dir+"/"+label+"-late-one-eye.csv",late_one) ||
     !run(label,"early",dir+"/"+label+"-early.csv",early)) return 2;
  if((label=="baseline" && (late.probes_late==0 || late_one.probes_late==0)) ||
     (label=="candidate" && (clean.probes_late==0 || late.probes_late!=0 || late.probes_recovery==0 || late.late_inputs==0 || late.final_target<500'000'000 ||
       late_one.probes_late!=0 || late_one.late_inputs==0 || late_one.probes_recovery==0 || late_one.final_target<500'000'000 ||
       early.probes_late==0 || early.late_inputs!=0 || clean.final_target!=early.final_target || clean.probes_late!=early.probes_late))) {
    std::fprintf(stderr,"gate failure: clean probes=%zu; late probes=%zu recovery probes=%zu target=%u; early probes=%zu target=%u\n",clean.probes_late,late.probes_late,late.probes_recovery,late.final_target,early.probes_late,early.final_target); return 1;
  }
  return 0;
}
