#define main existing_test_main
#include "bitrate_bbr_test.cpp"
#undef main
int main() {
  harness h(mode::bbr,24e6);
  h.quiet();
  auto steady=h.current();
  uint32_t late_max=0,late_min=UINT32_MAX;
  for(int i=0;i<1800;++i) {h.feed_frame(false,true);late_max=std::max(late_max,h.current());late_min=std::min(late_min,h.current());}
  std::printf("steady=%u late_min=%u late_max=%u\n",steady,late_min,late_max);
  h.capacity=48e6;
  h.seconds(20);
  auto recovered=h.quiet();
  std::printf("recovered=%u estimate=%u\n",recovered,h.ctl.bandwidth_estimate());
  CHECK(late_max<=steady+100);
  CHECK(late_min+100>=steady);
  CHECK(near(recovered,settled(48e6)));
  std::printf("%d checks %d failures\n",checks,failures);
  return failures?1:0;
}
