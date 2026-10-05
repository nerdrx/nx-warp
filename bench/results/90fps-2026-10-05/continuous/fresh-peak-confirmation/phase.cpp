#define main existing_test_main
#include "reference-bbr-harness.cpp"
#undef main
int main(int argc, char **argv)
{
 if (argc != 2) return 2;
 FILE *out = std::fopen(argv[1], "w");
 if (!out) return 2;
 std::fprintf(out, "burst_frame,frame,bitrate_bps,estimate_bps\n");
 for (int burst = -1; burst < 47; ++burst)
 {
  harness h(mode::bbr, 24e6);
  h.quiet();
  uint32_t low = h.current(), high = h.current();
  for (int i = 0; i < 270; ++i)
  {
   const auto bytes = h.frame_bytes();
   const auto wire = i == burst ? std::max<int64_t>(period * 0.4, int64_t(8e9 * double(bytes) / 48e6)) : h.wire_ns(bytes);
   h.feed_raw(bytes, wire);
   low = std::min(low, h.current());
   high = std::max(high, h.current());
   std::fprintf(out, "%d,%d,%u,%u\n", burst, i, h.current(), h.ctl.bandwidth_estimate());
  }
  std::printf("burst=%d min=%u max=%u final=%u\n", burst, low, high, h.current());
 }
 return std::fclose(out) == 0 ? 0 : 2;
}
