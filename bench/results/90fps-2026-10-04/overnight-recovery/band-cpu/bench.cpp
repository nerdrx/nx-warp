#include <zstd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <span>
#include <string>
#include <vector>

struct Image { uint32_t w{},h{},bx{},by{}; std::vector<uint8_t> raw; };
struct Region { uint32_t x{},y{},w{},h{}; std::vector<uint8_t> z; };
using Clock=std::chrono::steady_clock;
static uint32_t u24(const uint8_t*p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16);}
static Image load(const char *path){
 std::ifstream f(path,std::ios::binary); std::vector<uint8_t>b((std::istreambuf_iterator<char>(f)),{});
 if(b.size()<16||b[0]!=0x13||b[1]!=0xab||b[2]!=0xa1||b[3]!=0x5c) throw std::runtime_error("bad ASTC file");
 Image a; a.w=u24(b.data()+7);a.h=u24(b.data()+10);a.bx=(a.w+7)/8;a.by=(a.h+7)/8;
 a.raw.assign(b.begin()+16,b.end()); if(a.raw.size()!=size_t(a.bx)*a.by*16) throw std::runtime_error("bad ASTC raw size");return a;
}
static std::vector<uint8_t> compress(std::span<const uint8_t> in){std::vector<uint8_t> z(ZSTD_compressBound(in.size()));size_t n=ZSTD_compress(z.data(),z.size(),in.data(),in.size(),3);if(ZSTD_isError(n))throw std::runtime_error(ZSTD_getErrorName(n));z.resize(n);return z;}
static std::vector<Region> regions(const Image&a,uint32_t pixels){
 const uint32_t edge=(pixels+7)/8;std::vector<Region>r;
 for(uint32_t y=0;y<a.by;y+=edge)
  for(uint32_t x=0;x<a.bx;x+=edge){
   Region q{x,y,std::min(edge,a.bx-x),std::min(edge,a.by-y),{}};
   std::vector<uint8_t> bytes(size_t(q.w)*q.h*16);
   for(uint32_t row=0;row<q.h;row++)
    std::copy_n(a.raw.data()+(size_t(q.y+row)*a.bx+q.x)*16,size_t(q.w)*16,bytes.data()+size_t(row)*q.w*16);
   q.z=compress(bytes);r.push_back(std::move(q));
  }
 return r;
}
static bool decode_whole(ZSTD_DCtx*ctx,const std::vector<uint8_t>&z,std::span<uint8_t>out){size_t n=ZSTD_decompressDCtx(ctx,out.data(),out.size(),z.data(),z.size());return !ZSTD_isError(n)&&n==out.size();}
static bool decode_regions(ZSTD_DCtx*ctx,const Image&a,const std::vector<Region>&rs,std::span<uint8_t>out,std::span<uint8_t>tile,uint32_t skip_mod=0){
 for(size_t i=0;i<rs.size();++i){if(skip_mod&&i%skip_mod==0)continue;const auto&q=rs[i];size_t n=ZSTD_decompressDCtx(ctx,tile.data(),size_t(q.w)*q.h*16,q.z.data(),q.z.size());if(ZSTD_isError(n)||n!=size_t(q.w)*q.h*16)return false;for(uint32_t row=0;row<q.h;row++)std::copy_n(tile.data()+size_t(row)*q.w*16,size_t(q.w)*16,out.data()+(size_t(q.y+row)*a.bx+q.x)*16);}return true;
}
static std::pair<double,double> stats(std::vector<double>v){std::sort(v.begin(),v.end());return {v[v.size()/2],v[v.size()*95/100]};}
static size_t payload_sum(const std::vector<Region>&r){size_t n=0;for(auto&q:r)n+=q.z.size();return n;}
static size_t region_scratch_bytes(const std::vector<Region>&r){size_t n=0;for(auto&q:r)n=std::max(n,size_t(q.w)*q.h*16);return n;}
static void run_image(const char*name,const Image&a,uint32_t pixels,const std::vector<uint8_t>&whole,const std::vector<Region>&rs){
 std::vector<double>w,t;w.reserve(200);t.reserve(200);std::vector<uint8_t>out(a.raw.size()),tile(region_scratch_bytes(rs));ZSTD_DCtx*wc=ZSTD_createDCtx(),*tc=ZSTD_createDCtx();if(!wc||!tc)throw std::bad_alloc();
 for(unsigned it=0;it<220;it++){auto one=[&](bool tiled){auto b=Clock::now();bool ok=tiled?decode_regions(tc,a,rs,out,tile):decode_whole(wc,whole,out);auto e=Clock::now();if(!ok||out!=a.raw)throw std::runtime_error("full decode mismatch");if(it>=20)(tiled?t:w).push_back(std::chrono::duration<double,std::micro>(e-b).count());};if(it&1){one(true);one(false);}else{one(false);one(true);}}
 auto [wp,w95]=stats(w);auto[tp,t95]=stats(t);const size_t headers=rs.size()*8;
 std::printf("full %s region=%u px count=%zu raw=%zu whole_wire=%zu regional_payload=%zu regional_wire_est=%zu whole_dctx_us=%.1f/%.1f regions_dctx_us=%.1f/%.1f\n",name,pixels,rs.size(),a.raw.size(),whole.size()+24,payload_sum(rs),payload_sum(rs)+headers,wp,w95,tp,t95);ZSTD_freeDCtx(wc);ZSTD_freeDCtx(tc);
}
static void run_partial(const Image&old,const Image&now,uint32_t pixels,const std::vector<uint8_t>&whole,const std::vector<Region>&rs){
 if(old.w!=now.w||old.h!=now.h)throw std::runtime_error("partial dimension mismatch");
 std::vector<uint8_t>expected=old.raw,out=old.raw,tile(region_scratch_bytes(rs));ZSTD_DCtx*wc=ZSTD_createDCtx(),*pc=ZSTD_createDCtx();if(!wc||!pc)throw std::bad_alloc();
 size_t write_bytes=0;
 for(size_t i=0;i<rs.size();i++)if(i%10!=0){const auto&q=rs[i];write_bytes+=size_t(q.w)*q.h*16;for(uint32_t y=0;y<q.h;y++)std::copy_n(now.raw.data()+(size_t(q.y+y)*now.bx+q.x)*16,size_t(q.w)*16,expected.data()+(size_t(q.y+y)*now.bx+q.x)*16);}
 std::vector<double>times,whole_times;times.reserve(200);whole_times.reserve(200);
 for(unsigned it=0;it<220;it++){
  auto partial=[&]{out=old.raw;auto b=Clock::now();bool ok=decode_regions(pc,now,rs,out,tile,10);auto e=Clock::now();if(!ok||out!=expected)throw std::runtime_error("partial retained-buffer mismatch");if(it>=20)times.push_back(std::chrono::duration<double,std::micro>(e-b).count());};
  auto full=[&]{auto b=Clock::now();bool ok=decode_whole(wc,whole,out);auto e=Clock::now();if(!ok||out!=now.raw)throw std::runtime_error("partial comparator full decode mismatch");if(it>=20)whole_times.push_back(std::chrono::duration<double,std::micro>(e-b).count());};
  if(it&1){partial();full();}else{full();partial();}
 }
 auto[p,p95]=stats(times);auto[wp,wp95]=stats(whole_times);size_t payload=0,count=0;for(size_t i=0;i<rs.size();i++)if(i%10!=0){payload+=rs[i].z.size();count++;}
 std::printf("partial %ux%u region=%u updated=%zu/%zu raw_write=%zu regional_payload=%zu header_est=%zu full_dctx_us=%.1f/%.1f update_dctx_us=%.1f/%.1f\n",now.w,now.h,pixels,count,rs.size(),write_bytes,payload,payload+count*8,wp,wp95,p,p95);ZSTD_freeDCtx(wc);ZSTD_freeDCtx(pc);
}
int main(int argc,char**argv){try{if(argc!=3)throw std::runtime_error("usage: bench dark.astc forest.astc");auto dark=load(argv[1]),forest=load(argv[2]);if(dark.w!=forest.w||dark.h!=forest.h)throw std::runtime_error("fixture dimensions differ");for(uint32_t pixels:{256u,128u}){auto d=regions(dark,pixels),f=regions(forest,pixels);auto dz=compress(dark.raw),fz=compress(forest.raw);run_image("dark",dark,pixels,dz,d);run_image("forest",forest,pixels,fz,f);run_partial(dark,forest,pixels,fz,f);}return 0;}catch(const std::exception&e){std::fprintf(stderr,"ERROR: %s\n",e.what());return 2;}}
