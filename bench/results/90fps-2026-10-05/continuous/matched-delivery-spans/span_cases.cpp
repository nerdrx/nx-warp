#define main existing_test_main
#include "reference-bbr-harness.cpp"
#undef main
struct input { uint32_t bytes; int64_t ro,rs,so,ss; bool lost=false; int meta=1; };
uint32_t run(const std::vector<input>& in, bool quality=false, int duplicate=0, int64_t recv_origin=10'000'000'000LL,int64_t send_origin=20'000'000'000LL) {
 bitrate_controller c; auto now=tp{}+1h;c.configure({.enabled=true},1'000'000'000,true,false,mode::bbr);c.set_pacing_window(paced);
 for(uint64_t frame=0;frame<60;++frame) {
  for(uint8_t eye=0;eye<in.size();++eye){auto a=in[eye];c.on_frame_bytes(frame,eye,a.bytes,now,quality?24'000'000:0);
   wivrn::from_headset::feedback f{};f.frame_index=frame;f.stream_index=eye;
   f.received_first_packet=recv_origin+frame*20'000'000+a.ro;
   if(!a.lost){f.received_last_packet=f.received_first_packet+a.rs;f.sent_to_decoder=std::max(f.received_first_packet,f.received_last_packet)+1'000'000;f.received_from_decoder=f.sent_to_decoder+1'000'000;f.blitted=f.received_from_decoder+1'000'000;f.times_displayed=1;}
   if(a.meta){f.send_begin=send_origin+frame*20'000'000+a.so;f.send_end=f.send_begin+a.ss;if(a.meta==2)f.send_begin=0;if(a.meta==3)f.send_end=0;}
   if(duplicate==2){auto incomplete=f;incomplete.send_end=0;c.on_feedback(incomplete,period,true,now);incomplete=f;incomplete.send_begin=0;c.on_feedback(incomplete,period,true,now);continue;}
   c.on_feedback(f,period,true,now);
   if(duplicate){c.on_feedback(f,period,true,now);f.send_begin=f.send_end=0;c.on_feedback(f,period,true,now);}
  } now+=20ms;
 } return c.bandwidth_estimate();
}
int main(){
 using V=std::vector<input>;
 std::puts("case,rate_bps");
 auto emit=[](const char*n,const V&v,bool q=false,int d=0,int64_t ro=10'000'000'000LL,int64_t so=20'000'000'000LL){std::printf("%s,%u\n",n,run(v,q,d,ro,so));};
 emit("one-fast-receive",{{400000,0,6000000,0,12000000}});
 emit("receive-slower",{{400000,0,12000000,0,6000000}});
 emit("serial-send-overlap-receive",{{200000,0,6000000,0,6000000},{200000,0,6000000,6000000,6000000}});
 emit("serial-receive-overlap-send",{{200000,0,6000000,0,6000000},{200000,6000000,6000000,0,6000000}});
 emit("partial-send",{{200000,0,6000000,0,6000000},{200000,0,6000000,3000000,6000000}});
 emit("send-gap",{{200000,0,6000000,0,6000000},{200000,0,6000000,12000000,6000000}});
 emit("missing-one-send",{{200000,0,6000000,0,12000000},{200000,0,6000000,0,12000000,false,0}});
 emit("zero-send-begin",{{400000,0,6000000,0,12000000,false,2}});
 emit("zero-send-end",{{400000,0,6000000,0,12000000,false,3}});
 emit("equal-send",{{400000,0,6000000,0,0}});
 emit("reversed-send",{{400000,0,6000000,0,-1000000}});
 emit("unmatched-zero-byte-send",{{400000,0,6000000,0,12000000},{0,0,6000000,0,100000000}});
 emit("reversed-receive",{{200000,0,6000000,0,12000000},{200000,12000000,-1000000,0,100000000}});
 emit("app-limited-long-send",{{200000,0,500000,0,12000000}});
 emit("one-eye-lost",{{200000,0,6000000,0,12000000},{200000,0,6000000,0,12000000,true}});
 emit("duplicate-complete",{{400000,0,6000000,0,12000000}},false,true);
 emit("independent-origin-shift",{{200000,0,6000000,0,6000000},{200000,0,6000000,6000000,6000000}},false,false,9'000'000'000'000LL,1'000'000'000'000LL);
 emit("complementary-incomplete-send",{{400000,0,6000000,0,12000000}},false,2);
 emit("quality-path-unchanged",{{400000,0,6000000,0,12000000}},true);
}
