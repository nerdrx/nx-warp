// Deterministic input replay through production bitrate_controller; no socket/network model.
#include "driver/bitrate_controller.h"
#include "util/u_logging.h"
#include <cstdint>
#include <cstdio>
extern "C" void u_log(const char *, int, const char *, enum u_logging_level, const char *, ...) {}
extern "C" enum u_logging_level u_log_get_global_level(void) { return U_LOGGING_INFO; }
using namespace std::chrono_literals;
using tp = wivrn::bitrate_controller::clock::time_point;
constexpr int64_t period_ns = 11'111'111;
void run(const char *name, uint8_t streams, int64_t second_eye_offset_ns, uint32_t bytes_per_stream) {
  wivrn::bitrate_controller c;
  auto now = tp{} + 1h;
  c.configure({.enabled=true}, 1'000'000'000, true, false, wivrn::bitrate_controller::mode::bbr);
  c.set_pacing_window(0.4f);
  for (uint64_t f=0; f<60; ++f) {
    const XrTime base = 10'000'000'000LL + XrTime(f)*period_ns;
    for (uint8_t eye=0; eye<streams; ++eye) {
      c.on_frame_bytes(f, eye, bytes_per_stream, now);
      wivrn::from_headset::feedback fb{};
      fb.frame_index=f; fb.stream_index=eye;
      fb.received_first_packet=base+(eye ? second_eye_offset_ns : 0);
      fb.received_last_packet=fb.received_first_packet+6'000'000;
      fb.sent_to_decoder=fb.received_last_packet+1'000'000;
      c.on_feedback(fb, period_ns, true, now);
    }
    now += 12ms;
  }
  const uint64_t bytes = uint64_t(streams)*bytes_per_stream;
  const int64_t envelope_ns = 6'000'000 + (streams == 2 ? second_eye_offset_ns : 0);
  const double receiver_envelope_rate_mbps = 8e3*double(bytes)/double(envelope_ns);
  std::printf("%s,%u,%lld,%u,%llu,%.3f,%.3f,%u\n",name,unsigned(streams),(long long)second_eye_offset_ns,bytes_per_stream,(unsigned long long)bytes,receiver_envelope_rate_mbps,c.bandwidth_estimate()/1e6,c.current());
}
int main() {
  std::puts("case,streams,eye1_offset_ns,bytes_per_stream,frame_bytes,full_envelope_rate_mbps,controller_estimate_mbps,bitrate_bps");
  run("one_stream",1,0,400'000);
  run("two_eye_overlap",2,0,200'000);
  run("two_eye_disjoint",2,6'000'000,200'000);
}
