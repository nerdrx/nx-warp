#include "nxwarp_direct_zstd.h"
#include "nxwarp_direct_lz4.h"
#include <fstream>
#include <chrono>
#include <cstdio>
#include <cstdlib>
using namespace wivrn::nxwarp_direct;
int main(int argc,char**argv) {
 if(argc!=5) return 2;
 std::ifstream f(argv[1],std::ios::binary);
 std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),{}),lz,zs,out;
 layout l{uint32_t(std::stoul(argv[2])),uint32_t(std::stoul(argv[3])),2,true,256,false,true};
 if(raw.empty()||!parse_frame(l,raw)) return 3;
 auto a=compress_lz4(raw,lz),b=compress_zstd(raw,zs);
 for(auto method:{0,1}) {
  auto wire=method?b:a;
  std::vector<double> timings;
  for(int i=0;i<520;++i) {
   auto start=std::chrono::steady_clock::now();
   bool good=method?decompress_zstd(l,wire,out):decompress_lz4(l,wire,out);
   double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   if(!good || out!=raw) return 4;
   if(i>=20)timings.push_back(ms);
  }
  std::sort(timings.begin(),timings.end());
  std::printf("%s,%s,%zu,%zu,%.6f,%.6f,%.6f\n",argv[4],method?"zstd3":"lz4",raw.size(),wire.size(),timings[250],timings[475],timings[495]);
 }
}
