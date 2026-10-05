#include "astcenc_internal.h"
#include <zstd.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static constexpr unsigned W=2176,H=2176,BW=W/8,BH=H/8,NB=BW*BH,BYTES=NB*16,MODE=0x0F3;
using Bytes=std::vector<uint8_t>;
static Bytes load(const char*p,size_t n){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error(std::string("open ")+p);Bytes b(n);f.read((char*)b.data(),n);if(size_t(f.gcount())!=n)throw std::runtime_error(std::string("short read ")+p);return b;}
static uint32_t getbits(const uint8_t*p,unsigned off,unsigned n){uint32_t v=0;for(unsigned i=0;i<n;i++)v|=uint32_t((p[(off+i)>>3]>>((off+i)&7))&1u)<<i;return v;}
static void setbits(uint8_t*p,unsigned off,unsigned n,uint32_t v){for(unsigned i=0;i<n;i++){uint8_t&m=p[(off+i)>>3];uint8_t bit=uint8_t(1u<<((off+i)&7));m=uint8_t((m&~bit)|(((v>>i)&1u)?bit:0));}}
static uint8_t expand6(uint32_t q){return uint8_t((q<<2)|(q>>4));}
static void decode(astcenc_context*c,const Bytes&blocks,Bytes&rgba){auto r=astcenc_decompress_reset(c);if(r!=ASTCENC_SUCCESS)throw std::runtime_error(astcenc_get_error_string(r));void*sl[]={rgba.data()};astcenc_image im{W,H,1,ASTCENC_TYPE_U8,sl};astcenc_swizzle sw{ASTCENC_SWZ_R,ASTCENC_SWZ_G,ASTCENC_SWZ_B,ASTCENC_SWZ_A};auto e=astcenc_decompress_image(c,blocks.data(),blocks.size(),&im,&sw,0);if(e!=ASTCENC_SUCCESS)throw std::runtime_error(astcenc_get_error_string(e));}
static uint64_t sse(const Bytes&a,const Bytes&b){uint64_t e=0;for(size_t p=0;p<a.size();p+=4)for(unsigned k=0;k<3;k++){int d=int(a[p+k])-int(b[p+k]);e+=uint64_t(d*d);}return e;}
static size_t zbytes(ZSTD_CCtx*c,const Bytes&b){Bytes o(ZSTD_compressBound(b.size()));size_t n=ZSTD_compress2(c,o.data(),o.size(),b.data(),b.size());if(ZSTD_isError(n))throw std::runtime_error(ZSTD_getErrorName(n));return n;}
struct counts {unsigned ordinary=0, other=0, unchanged=0, flips=0, saturated=0, changed=0;};
static Bytes lower_q6_codes(const Bytes&base,const block_size_descriptor&bsd,counts&n){
 Bytes out=base;for(unsigned i=0;i<NB;i++){uint8_t*p=out.data()+size_t(i)*16;const uint8_t*src=base.data()+size_t(i)*16;symbolic_compressed_block b{};physical_to_symbolic(bsd,src,b);
  if(b.block_type==SYM_BTYPE_ERROR)throw std::runtime_error("invalid baseline ASTC block");
  if(b.block_type!=SYM_BTYPE_NONCONST||b.block_mode!=MODE||b.partition_count!=1||b.color_formats[0]!=FMT_RGB||get_quant_level(b.quant_mode)!=64){n.other++;continue;}n.ordinary++;
  // Production shader's CEM8/Q64 direct RGB fields: e0.r,e1.r,e0.g,e1.g,e0.b,e1.b.
  constexpr unsigned offsets[6]={17,23,29,35,41,47};uint32_t q[6];for(unsigned c=0;c<6;c++)q[c]=getbits(src,offsets[c],6);
  bool rh=false,ah=false;vint4 e0,e1;unpack_color_endpoints(ASTCENC_PRF_LDR,FMT_RGB,b.color_values[0],rh,ah,e0,e1);
  if(rh||ah)throw std::runtime_error("unexpected HDR direct endpoint");
  const uint8_t decoded[6]={uint8_t(e0.lane<0>()/257),uint8_t(e1.lane<0>()/257),uint8_t(e0.lane<1>()/257),uint8_t(e1.lane<1>()/257),uint8_t(e0.lane<2>()/257),uint8_t(e1.lane<2>()/257)};
  for(unsigned c=0;c<6;c++)if(decoded[c]!=expand6(q[c]))throw std::runtime_error("raw endpoint bit mapping disagrees with astcenc");
  uint32_t v[6];for(unsigned c=0;c<6;c++){v[c]=std::min(((q[c]+1u)/2u)*2u,63u);if(v[c]==63u&&q[c]!=63u)n.saturated++;}
  unsigned sum0=expand6(v[0])+expand6(v[2])+expand6(v[4]),sum1=expand6(v[1])+expand6(v[3])+expand6(v[5]);
  if(sum0>sum1){n.flips++;n.unchanged++;continue;} // Keep baseline to prevent CEM8 blue contraction.
  bool any=false;for(unsigned c=0;c<6;c++){if(q[c]!=v[c])any=true;setbits(p,offsets[c],6,v[c]);}
  if(any)n.changed++;else n.unchanged++;
  for(unsigned bit=0;bit<128;bit++){bool endpoint=false;for(unsigned c=0;c<6;c++)endpoint|=bit>=offsets[c]&&bit<offsets[c]+6;if(!endpoint&&getbits(src,bit,1)!=getbits(p,bit,1))throw std::runtime_error("non-endpoint bit changed");}
  symbolic_compressed_block check{};physical_to_symbolic(bsd,p,check);if(check.block_type!=SYM_BTYPE_NONCONST||check.block_mode!=MODE||check.partition_count!=1||check.color_formats[0]!=FMT_RGB||get_quant_level(check.quant_mode)!=64)throw std::runtime_error("candidate mode/endpoint parse changed");
 }
 return out;
}
static void run(const char*name,const char*inpath,const char*basepath,block_size_descriptor&bsd,astcenc_context*ctx,ZSTD_CCtx*z){Bytes src=load(inpath,size_t(W)*H*4),base=load(basepath,BYTES),candidate,dec0(src.size()),dec1(src.size());counts n;candidate=lower_q6_codes(base,bsd,n);decode(ctx,base,dec0);decode(ctx,candidate,dec1);uint64_t e0=sse(src,dec0),e1=sse(src,dec1);double denom=double(W)*H*3, mse0=double(e0)/denom,mse1=double(e1)/denom;size_t z0=zbytes(z,base),z1=zbytes(z,candidate);double p0=10*std::log10(255.0*255.0/mse0),p1=10*std::log10(255.0*255.0/mse1);double save=100.0*(double(z0)-double(z1))/double(z0);std::cout<<name<<","<<NB<<","<<n.ordinary<<","<<n.other<<","<<n.changed<<","<<n.unchanged<<","<<n.flips<<","<<n.saturated<<","<<e0<<","<<e1<<","<<mse0<<","<<mse1<<","<<p0<<","<<p1<<","<<z0<<","<<z1<<","<<z0+24<<","<<z1+24<<","<<save<<"\n";}
int main(int argc,char**argv){try{if(argc!=5){std::cerr<<"usage: gate INPUT0 ASTC0 INPUT1 ASTC1\n";return 2;}astcenc_config cfg{};if(astcenc_config_init(ASTCENC_PRF_LDR,8,8,1,ASTCENC_PRE_MEDIUM,0,&cfg)!=ASTCENC_SUCCESS)return 3;astcenc_context*ctx=nullptr;if(astcenc_context_alloc(&cfg,1,&ctx)!=ASTCENC_SUCCESS)return 4;auto*bsd=new block_size_descriptor{};init_block_size_descriptor(8,8,1,false,4,1.0f,*bsd);ZSTD_CCtx*z=ZSTD_createCCtx();if(!z)throw std::bad_alloc();size_t e=ZSTD_CCtx_setParameter(z,ZSTD_c_compressionLevel,3);if(ZSTD_isError(e))throw std::runtime_error(ZSTD_getErrorName(e));std::cout<<"eye,blocks,ordinary_cem8_q64,other_unchanged,changed_blocks,unchanged_ordinary,blue_order_flip_guard,saturated63,baseline_rgb_sse,candidate_rgb_sse,baseline_mse,candidate_mse,baseline_psnr_db,candidate_psnr_db,baseline_zstd3,candidate_zstd3,baseline_packet24,candidate_packet24,zstd_saved_percent\n";run("dark",argv[1],argv[2],*bsd,ctx,z);run("forest",argv[3],argv[4],*bsd,ctx,z);ZSTD_freeCCtx(z);astcenc_context_free(ctx);delete bsd;return 0;}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
