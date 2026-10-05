#define main existing_test_main
#include "reference-bbr-harness.cpp"
#undef main
int main(int argc,char**argv) {
 if(argc!=2)return 2;
 FILE *out=std::fopen(argv[1],"w");if(!out)return 2;
 std::fprintf(out,"scenario,frame,bitrate_bps,wire_ns,estimate_bps\n");
 for (std::string scenario : {"clean-rise","single-high","repeated-high","app-limited","loss-after-rise","no-feedback"}) {
  harness h(mode::bbr,24e6);
  h.quiet();
  const uint32_t before=h.current();
  if(scenario=="clean-rise" || scenario=="loss-after-rise")h.capacity=48e6;
  uint32_t low=before,high=before;
  for(int i=0;i<900;++i) {
   const auto bytes=h.frame_bytes();
   auto wire=h.wire_ns(bytes);
   const bool burst=(scenario=="single-high" && i==23) || (scenario=="repeated-high" && i%23==0);
   if(burst)wire=std::max<int64_t>(period*0.4,int64_t(8e9*double(bytes)/48e6));
   if(scenario=="app-limited")h.feed_raw(128,400);
   else if(scenario=="no-feedback")h.advance(std::chrono::nanoseconds(period));
   else h.feed_raw(bytes,wire,scenario=="loss-after-rise" && i<20);
   low=std::min(low,h.current());high=std::max(high,h.current());
   std::fprintf(out,"%s,%d,%u,%lld,%u\n",scenario.c_str(),i,h.current(),(long long)wire,h.ctl.bandwidth_estimate());
  }
  std::printf("%s before=%u min=%u max=%u final=%u estimate=%u\n",scenario.c_str(),before,low,high,h.current(),h.ctl.bandwidth_estimate());
 }
 if(std::fclose(out)!=0)return 2;
 return 0;
}
