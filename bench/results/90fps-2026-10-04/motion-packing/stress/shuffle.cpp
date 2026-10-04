#include <zstd.h>
#include <vector>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <stdexcept>
using Clock=std::chrono::steady_clock;
int main(int argc,char**argv){
 if(argc<2)return 1;puts("fixture,lane_bytes,compressed_bytes,pack_zstd_p50_ms,unpack_p50_ms,exact");
 ZSTD_CCtx* c=ZSTD_createCCtx();ZSTD_DCtx*d=ZSTD_createDCtx();
 for(int a=1;a<argc;a++){
  std::ifstream f(argv[a],std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(f)),{});
  if(b.size()<16||(b.size()-16)%16)throw std::runtime_error("ASTC fixture");
  std::vector<uint8_t>raw(b.begin()+16,b.end()),sh(raw.size()),z(ZSTD_compressBound(raw.size())),un(raw.size()),out(raw.size());
  size_t blocks=raw.size()/16;
  for(size_t lane : {16,8,4,2,1}){
   std::vector<double>et,dt;size_t n=0;
   for(int i=0;i<35;i++){
    auto t=Clock::now();
    if(lane==16)sh=raw;else for(size_t j=0;j<16/lane;j++)for(size_t k=0;k<blocks;k++)memcpy(sh.data()+(j*blocks+k)*lane,raw.data()+k*16+j*lane,lane);
    n=ZSTD_compressCCtx(c,z.data(),z.size(),sh.data(),sh.size(),3);auto e=Clock::now();
    if(ZSTD_isError(n)||ZSTD_decompressDCtx(d,un.data(),un.size(),z.data(),n)!=raw.size())throw std::runtime_error("zstd");
    auto u=Clock::now();
    if(lane==16)out=un;else for(size_t j=0;j<16/lane;j++)for(size_t k=0;k<blocks;k++)memcpy(out.data()+k*16+j*lane,un.data()+(j*blocks+k)*lane,lane);
    auto v=Clock::now();if(out!=raw)throw std::runtime_error("exact");
    if(i>=5){et.push_back(std::chrono::duration<double,std::milli>(e-t).count());dt.push_back(std::chrono::duration<double,std::milli>(v-u).count());}
   }
   std::sort(et.begin(),et.end());std::sort(dt.begin(),dt.end());printf("%s,%zu,%zu,%.6f,%.6f,true\n",argv[a],lane,n,et[15],dt[15]);
  }
 }
 ZSTD_freeCCtx(c);ZSTD_freeDCtx(d);
}