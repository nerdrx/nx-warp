#include "fec.h"
#include "shard_history.h"
#include "wivrn_packets.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

using namespace wivrn;
using shard_t = to_headset::video_stream_data_shard;

static shard_t::view_info_t view_info()
{
    return {.display_time = 42,
        .pose = {XrPosef{{0,0,0,1},{0.03f,0,0}}, XrPosef{{0,0,0,1},{-0.03f,0,0}}},
        .fov = {XrFovf{-0.9f,0.9f,0.9f,-0.9f}, XrFovf{-0.9f,0.9f,0.9f,-0.9f}},
        .foveation = {}, .alpha = false, .quad = {}};
}

static shard_t make_shard(uint64_t frame, uint16_t idx, std::span<uint8_t> payload, uint16_t count)
{
    shard_t s{};
    s.stream_item_idx = 0;
    s.frame_idx = frame;
    s.shard_idx = idx;
    if (idx == 0) s.view_info = view_info();
    if (idx + 1 == count) s.timing_info = shard_t::timing_info_t{1,2,3,4};
    s.payload = payload;
    return s;
}

static bool check(bool ok, const char * what)
{
    if (!ok) std::fprintf(stderr, "FAIL: %s\n", what);
    return ok;
}

static bool correctness()
{
    constexpr uint16_t n = 100;
    std::vector<uint8_t> data(100 * 1300);
    for (size_t i=0;i<data.size();++i) data[i]=uint8_t(i*37u+11u);
    shard_history a,b; a.set_enabled(true); b.set_enabled(true);
    std::vector<uint8_t> flat; flat.reserve(1400);
    for (uint16_t i=0;i<n;++i) {
        auto s=make_shard(9,i,std::span<uint8_t>(data).subspan(size_t(i)*1300,1300),n);
        fec::encode_blob(s,flat); a.push(9,i,flat,true);
        auto & spans=fec::encode_blob_spans(s); b.push_spans(9,i,spans,true);
    }
    std::vector<shard_history::hit> ha,hb;
    std::array<uint8_t,13> all{}; all.fill(0xff);
    a.collect(9,0,all,n,ha); b.collect(9,0,all,n,hb);
    if (!check(ha.size()==n && hb.size()==n,"equal collection count")) return false;
    for (size_t i=0;i<n;++i) {
        if (!check(ha[i].blob==hb[i].blob,"exact encoded bytes")) return false;
        auto decoded=fec::decode_blob(0,9,uint16_t(i),hb[i].blob);
        if (!check(decoded.payload.size()==1300 && std::equal(decoded.payload.begin(),decoded.payload.end(),data.begin()+i*1300),"payload decode")) return false;
        if (i==0 && !check(decoded.view_info.has_value(),"view_info decode")) return false;
        if (i+1==n && !check(decoded.timing_info && decoded.timing_info->send_end==4,"timing_info decode")) return false;
    }
    // Ring wrap/eviction: both implementations must stop returning overwritten frame 0.
    shard_history wa,wb; wa.set_enabled(true); wb.set_enabled(true);
    std::vector<uint8_t> big(1300,0x5a);
    for(uint64_t i=0;i<1800;++i) {
        auto s=make_shard(i,0,big,1);
        fec::encode_blob(s,flat); wa.push(i,0,flat,true);
        auto & spans=fec::encode_blob_spans(s); wb.push_spans(i,0,spans,true);
    }
    std::vector<shard_history::hit> x,y;
    std::array<uint8_t,1> bit{1};
    wa.collect(0,0,bit,1,x); wb.collect(0,0,bit,1,y);
    if(!check(x.empty()&&y.empty(),"wrapped oldest frame evicted")) return false;
    wa.collect(1799,0,bit,1,x); wb.collect(1799,0,bit,1,y);
    if(!check(x.size()==1&&y.size()==1&&x[0].blob==y[0].blob,"wrapped newest frame retained")) return false;
    // Disabled and secondary-path pushes remain absent.
    shard_history off; auto s=make_shard(1,0,big,1); auto & spans=fec::encode_blob_spans(s);
    off.push_spans(1,0,spans,true); off.set_enabled(true); off.push_spans(1,0,spans,false);
    x.clear(); off.collect(1,0,bit,1,x);
    if(!check(x.empty(),"disabled/secondary ignored")) return false;
    // One toggling writer and concurrent readers exercise ring ownership/locking.
    shard_history c; c.set_enabled(true); std::atomic<bool> fail=false;
    std::thread toggler([&]{ for(int i=0;i<100;++i){c.set_enabled(false);c.set_enabled(true);} });
    std::thread reader([&]{ std::vector<shard_history::hit> hits; for(int j=0;j<12000;++j){hits.clear(); c.collect(uint64_t(j%5000),0,bit,1,hits); for(auto &h:hits) try { (void)fec::decode_blob(0,j,h.shard_idx,h.blob); } catch(...) { fail=true; } } });
    for(uint64_t i=0;i<5000;++i){ auto q=make_shard(i,0,big,1); auto & p=fec::encode_blob_spans(q); c.push_spans(i,0,p,true); }
    reader.join(); toggler.join();
    return check(!fail.load(),"concurrent producer/reader/toggle decode");
}

static double cpu_seconds()
{
    timespec t{}; clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t); return double(t.tv_sec)+double(t.tv_nsec)*1e-9;
}
struct sample { double wall_ms,cpu_ms; uint64_t bytes; };
static sample run_frame(bool direct, uint64_t frame, shard_history & history,
                        std::vector<uint8_t> & payload, std::vector<uint8_t> & flat)
{
    constexpr size_t shard_bytes=1400;
    const uint16_t count=uint16_t((payload.size()+shard_bytes-1)/shard_bytes);
    const auto w0=std::chrono::steady_clock::now(); const double c0=cpu_seconds();
    uint64_t bytes=0;
    for(uint16_t i=0;i<count;++i) {
        size_t off=size_t(i)*shard_bytes, len=std::min(shard_bytes,payload.size()-off);
        auto s=make_shard(frame,i,std::span<uint8_t>(payload).subspan(off,len),count);
        if(direct) { auto & spans=fec::encode_blob_spans(s); for(auto p:spans) bytes+=p.size(); history.push_spans(frame,i,spans,true); }
        else { fec::encode_blob(s,flat); bytes+=flat.size(); history.push(frame,i,flat,true); }
    }
    const double cpu=cpu_seconds()-c0;
    const double wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-w0).count();
    return {wall,cpu*1000,bytes};
}
static void print_samples(const char * name, const std::vector<sample>& v)
{
    std::vector<double>w,c; uint64_t b=0; for(auto s:v){w.push_back(s.wall_ms);c.push_back(s.cpu_ms);b+=s.bytes;}
    std::sort(w.begin(),w.end()); std::sort(c.begin(),c.end());
    std::fprintf(stderr,"%s,n=%zu,wall_p50_ms=%.4f,wall_p95_ms=%.4f,cpu_p50_ms=%.4f,cpu_p95_ms=%.4f,blob_bytes=%llu\n",name,v.size(),w[(w.size()-1)*50/100],w[(w.size()-1)*95/100],c[(c.size()-1)*50/100],c[(c.size()-1)*95/100],(unsigned long long)b);
}
static void timing()
{
    // 500 Mbit/s aggregate stereo at 90 Hz: 347,222 payload bytes per eye/frame.
    std::vector<uint8_t> payload(347222); for(size_t i=0;i<payload.size();++i) payload[i]=uint8_t(i*13u+7u);
    std::vector<uint8_t> flat; flat.reserve(1440); shard_history a,b; a.set_enabled(true); b.set_enabled(true);
    for(uint64_t i=0;i<20;++i){run_frame(i&1,i,a,payload,flat);run_frame(!(i&1),i,b,payload,flat);}
    std::vector<sample> legacy,direct; legacy.reserve(30); direct.reserve(30);
    struct row { int pair, order; const char * treatment; sample value; };
    std::vector<row> rows; rows.reserve(60);
    uint64_t frame=20;
    for(int i=0;i<30;++i) {
        if(i&1){auto d=run_frame(true,frame++,b,payload,flat); direct.push_back(d); rows.push_back({i,0,"direct",d}); auto l=run_frame(false,frame++,a,payload,flat); legacy.push_back(l); rows.push_back({i,1,"legacy",l});}
        else {auto l=run_frame(false,frame++,a,payload,flat); legacy.push_back(l); rows.push_back({i,0,"legacy",l}); auto d=run_frame(true,frame++,b,payload,flat); direct.push_back(d); rows.push_back({i,1,"direct",d});}
    }
    std::printf("pair,order,treatment,wall_ms,process_cpu_ms,blob_bytes\n");
    for(const auto & r: rows) std::printf("%d,%d,%s,%.6f,%.6f,%llu\n",r.pair,r.order,r.treatment,r.value.wall_ms,r.value.cpu_ms,(unsigned long long)r.value.bytes);
    print_samples("legacy_encode_blob_then_ring_push",legacy); print_samples("direct_spans_into_ring",direct);
}
int main(int argc,char**argv)
{
    if(argc>1 && std::string_view(argv[1])=="time"){timing();return 0;}
    return correctness()?0:1;
}
