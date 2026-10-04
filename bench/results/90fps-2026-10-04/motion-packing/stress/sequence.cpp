#include "nxastc_motion.h"
#include <zstd.h>
#include <vector>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <chrono>
#include <stdexcept>
using namespace wivrn::nxastc_packet;
using Clock=std::chrono::steady_clock;
std::vector<uint8_t> read(const char* path) {
 std::ifstream f(path,std::ios::binary); if(!f) throw std::runtime_error(path);
 std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),{});
 if(b.size()!=16+64*64*16 || b[4]!=8 || b[5]!=8) throw std::runtime_error("fixture grid");
 return {b.begin()+16,b.end()};
}
int main(int argc,char**argv) {
 if(argc!=2)return 1;
 std::vector<std::vector<uint8_t>> frames;
 for(int i=0;i<36;i++){char p[4096]; snprintf(p,sizeof(p),"%s/%02d.astc",argv[1],i); frames.push_back(read(p));}
 ZSTD_CCtx* ctx=ZSTD_createCCtx(); if(!ctx)return 2;
 std::vector<uint8_t> packed(64*64*17), restored(64*64*16), decoded(packed.size()), z(ZSTD_compressBound(packed.size()));
 puts("gap,frame,mode,anchor_bytes,delta_bytes,admitted,pack_zstd_ms,exact");
 for(int gap : {1,2,4,8}) for(int i=gap;i<36;i++) for(int mode=0;mode<2;mode++) {
  auto& cur=frames[i]; auto& ref=frames[i-gap];
  size_t a=ZSTD_compressCCtx(ctx,z.data(),z.size(),cur.data(),cur.size(),3);
  if(ZSTD_isError(a))throw std::runtime_error("anchor zstd");
  auto t=Clock::now();
  if(mode==0) {if(!encode_motion_blocks(64,64,ref,cur,packed))throw std::runtime_error("pack");}
  else {std::fill(packed.begin(),packed.begin()+4096,4);for(size_t j=0;j<cur.size();j++)packed[4096+j]=cur[j]^ref[j];}
  size_t n=ZSTD_compressCCtx(ctx,z.data(),z.size(),packed.data(),packed.size(),3);
  double ms=std::chrono::duration<double,std::milli>(Clock::now()-t).count();
  if(ZSTD_isError(n))throw std::runtime_error("delta zstd");
  size_t d=ZSTD_decompress(decoded.data(),decoded.size(),z.data(),n);
  if(d!=packed.size() || !reconstruct_motion_blocks(64,64,ref,decoded,restored) || restored!=cur)throw std::runtime_error("exact roundtrip");
  printf("%d,%d,%s,%zu,%zu,%d,%.6f,true\n",gap,i,mode==0?"neighbors":"same_position",a,n,n*100<=a*85,ms);
 }
 ZSTD_freeCCtx(ctx);
}
