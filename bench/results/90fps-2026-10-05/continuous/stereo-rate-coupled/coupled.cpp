// Event replay through the production controller and the production pacing_slot arithmetic.
#include "driver/bitrate_controller.h"
#include "encoder/shard_pacer.h"
#include "util/u_logging.h"
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string_view>

extern "C" void u_log(const char *, int, const char *, enum u_logging_level, const char *, ...) {}
extern "C" enum u_logging_level u_log_get_global_level(void) { return U_LOGGING_INFO; }

using controller = wivrn::bitrate_controller;
using tp = controller::clock::time_point;
constexpr int64_t period_ns = 11'111'111; // Nominal 90 Hz controller period.
constexpr float pacing_window = 0.4f;    // configuration::pacing_config default.
constexpr uint64_t bps_to_ns = 8'000'000'000ull;
constexpr size_t group_bytes = wivrn::shard_pacer::group_bytes;

struct run_result { uint32_t bitrate; uint32_t estimate; int64_t last_start; int64_t last_end; };

bool run(const char *mode, uint64_t capacity_bps, const char *path, run_result &result)
{
  controller ctl;
  ctl.configure({.enabled=true}, 1'000'000'000, true, false, controller::mode::bbr);
  ctl.set_pacing_window(pacing_window);
  wivrn::pacing_slot slot; // The source has one slot shared by every video stream on this socket.
  int64_t previous_end = 0;
  uint64_t last_desired = 0;
  std::FILE *out = std::fopen(path,"w");
  if (!out) return false;
  std::fprintf(out,"model,capacity_bps,frame,bitrate_bps,frame_payload_bytes,desired_start_ns,actual_start_ns,eye0_budget_ns,eye0_span_ns,eye1_budget_ns,eye1_span_ns,serial_frame_ns,max_eye_utilisation,estimate_bps,next_target_bps\n");

  for (uint64_t frame=0; frame<1200; ++frame)
  {
    const uint64_t desired = frame*uint64_t(period_ns);
    const int64_t start = std::max<int64_t>(int64_t(desired), previous_end);
    const uint32_t bitrate = ctl.current();
    const uint64_t payload = uint64_t(double(bitrate)*double(period_ns)/8e9);
    const uint32_t left_bytes = uint32_t(payload/2), right_bytes=uint32_t(payload-left_bytes);

    auto eye_span = [&](uint32_t bytes, int64_t begin, size_t queued, int64_t &budget) {
      budget=slot.begin_frame(begin,period_ns,pacing_window,queued);
      const bool active=wivrn::shard_pacer(begin,budget,bytes).active();
      const uint64_t serial_ns=(uint64_t(bytes)*bps_to_ns+capacity_bps-1)/capacity_bps;
      return int64_t(std::max<uint64_t>(active?uint64_t(budget):0,serial_ns));
    };

    // Both eye jobs are ready together. The shared sender pops eye 0 with one eye queued,
    // then eye 1 with none queued. This is the explicit queue/readiness model for this replay.
    int64_t eye0_budget=0,eye1_budget=0;
    const int64_t eye0_start=start;
    const int64_t eye0_span=eye_span(left_bytes,eye0_start,1,eye0_budget);
    const int64_t eye1_start=eye0_start+eye0_span;
    const int64_t eye1_span=eye_span(right_bytes,eye1_start,0,eye1_budget);
    if (start < int64_t(desired) || eye0_span < 0 || eye1_span < 0 || eye1_start != eye0_start + eye0_span)
      return false;
    const int64_t eye0_end=eye0_start+eye0_span, eye1_end=eye1_start+eye1_span;
    const XrTime base=1'000'000'000LL+XrTime(eye0_start);

    for (uint8_t stream=0;stream<2;++stream)
    {
      const int64_t first=stream?eye1_start:eye0_start;
      const int64_t last=stream?eye1_end:eye0_end;
      const uint32_t bytes=stream?right_bytes:left_bytes;
      const tp now=tp{}+std::chrono::hours(1)+std::chrono::nanoseconds(last);
      ctl.on_frame_bytes(frame,stream,bytes,now);
      wivrn::from_headset::feedback fb{};
      fb.frame_index=frame; fb.stream_index=stream;
      fb.received_first_packet=base+XrTime(first-eye0_start);
      fb.received_last_packet=base+XrTime(last-eye0_start);
      fb.sent_to_decoder=fb.received_last_packet+1'000'000;
      ctl.on_feedback(fb,period_ns,true,now);
    }
    previous_end=eye1_end;
    last_desired=desired;
    const double max_util=double(std::max(eye0_span,eye1_span))/period_ns;
    std::fprintf(out,"%s,%llu,%llu,%u,%llu,%llu,%lld,%lld,%lld,%lld,%lld,%lld,%.6f,%u,%u\n",mode,
      (unsigned long long)capacity_bps,(unsigned long long)frame,bitrate,(unsigned long long)payload,
      (unsigned long long)desired,(long long)start,(long long)eye0_budget,(long long)eye0_span,
      (long long)eye1_budget,(long long)eye1_span,(long long)(eye1_end-start),max_util,
      ctl.bandwidth_estimate(),ctl.current());
  }
  if (std::fclose(out) != 0) return false;
  result={ctl.current(),ctl.bandwidth_estimate(),int64_t(last_desired),previous_end};
  std::printf("%s capacity=%llu final_target=%u estimate=%u last_frame_start=%lld last_end=%lld\n",mode,
    (unsigned long long)capacity_bps,result.bitrate,result.estimate,(long long)result.last_start,(long long)result.last_end);
  return true;
}

int main(int argc,char **argv)
{
  if (argc!=4) return 2;
  run_result limited{}, wide{};
  if (!run(argv[1],500'000'000,argv[2],limited) || !run("wide",2'000'000'000,argv[3],wide)) return 1;
  if (std::string_view(argv[1]) == "patched" &&
      (limited.bitrate > 500'000'000 || wide.bitrate != 1'000'000'000)) return 1;
  return 0;
}
