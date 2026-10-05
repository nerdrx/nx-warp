// Synthetic delivery-time perturbations; actual controller, virtual 90 Hz clock.
#define main existing_test_main
#include "reference-bbr-harness.cpp"
#undef main
int main(int argc, char **argv) {
 if(argc!=2)return 2;
 FILE *out=std::fopen(argv[1],"w"); if(!out)return 2;
 std::fprintf(out,"scenario,phase,frame,elapsed_ns,input_capacity_bps,feedback,bytes,wire_ns,bitrate_bps,estimate_bps\n");
 for (const std::string scenario : {"clean","tiny-records","records-then-collapse","moderate-125","moderate-150","repeated-125","repeated-150","gap-1-stable","gap-5-stable","gap-12-stable","gap-1-rise","gap-5-rise","gap-12-rise","app-limited-12","loss-after-records","record-anomalies-11","record-anomalies-11-collapse"}) {
  for(int phase: {0,360}) {
   harness h(mode::bbr,24e6); h.quiet(); h.feed(phase);
   auto start=h.now;
   for(int i=0;i<1800;++i) {
    if(i==45 && scenario.starts_with("gap-")) {
     int gap=scenario.starts_with("gap-12")?12:scenario.starts_with("gap-5")?5:1;
     h.advance(std::chrono::seconds(gap));
     if(scenario.ends_with("rise"))h.capacity=48e6;
    }
    const bool app=scenario=="app-limited-12" && i<1080;
    double supplied=h.capacity;
    if((scenario=="tiny-records" || scenario=="records-then-collapse" || scenario=="loss-after-records") && i<360)
     supplied=24e6*(1.0+0.00001*(i+1)); // each timestamp implies a new maximum, +0.36% total
    if(scenario=="records-then-collapse" && i>=360){h.capacity=12e6; supplied=h.capacity;}
    if(scenario=="record-anomalies-11-collapse" && i>=360){h.capacity=12e6; supplied=h.capacity;}
    if(scenario.starts_with("record-anomalies-11") && i%11==0)supplied=48e6*(1.0+0.00001*(i+1));
    const bool burst=((scenario=="moderate-125"||scenario=="moderate-150")&&i==38)
     || ((scenario=="repeated-125"||scenario=="repeated-150")&&i%23==0);
    if(burst)supplied=24e6*(scenario.ends_with("125")?1.25:1.50);
    auto bytes=app?uint64_t(128):h.frame_bytes();
    auto wire=app?int64_t(400):std::max<int64_t>(int64_t(period*0.4),int64_t(8e9*double(bytes)/supplied));
    bool lost=scenario=="loss-after-records" && i>=360 && i<380;
    h.feed_raw(bytes,wire,lost);
    std::fprintf(out,"%s,%d,%d,%lld,%.0f,%d,%llu,%lld,%u,%u\n",scenario.c_str(),phase,i,(long long)std::chrono::duration_cast<std::chrono::nanoseconds>(h.now-start).count(),supplied,lost?0:1,(unsigned long long)bytes,(long long)wire,h.current(),h.ctl.bandwidth_estimate());
   }
  }
 }
 return std::fclose(out)==0?0:2;
}
