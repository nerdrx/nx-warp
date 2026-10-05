#include "astcenc_internal.h"
#include <zstd.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static constexpr unsigned W=2176,H=2176,BW=W/8,BH=H/8,NB=BW*BH,BYTES=NB*16;
static constexpr unsigned CANDIDATE_MODE=0x544;
using Bytes=std::vector<uint8_t>;
static Bytes load(const char*p,size_t n){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error(std::string("open ")+p);Bytes b(n);f.read((char*)b.data(),n);if(size_t(f.gcount())!=n)throw std::runtime_error(std::string("short read ")+p);if(f.peek()!=std::char_traits<char>::eof())throw std::runtime_error(std::string("oversized input ")+p);return b;}
static void decode(astcenc_context*ctx,const Bytes&astc,Bytes&rgba){void*sl[]={rgba.data()};astcenc_image im{W,H,1,ASTCENC_TYPE_U8,sl};astcenc_swizzle sw{ASTCENC_SWZ_R,ASTCENC_SWZ_G,ASTCENC_SWZ_B,ASTCENC_SWZ_A};auto e=astcenc_decompress_image(ctx,astc.data(),astc.size(),&im,&sw,0);if(e!=ASTCENC_SUCCESS)throw std::runtime_error(astcenc_get_error_string(e));}
static inline uint64_t sse_block(const Bytes&a,const Bytes&b,unsigned bx,unsigned by){uint64_t e=0;for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){size_t p=((by*8+y)*W+bx*8+x)*4;for(unsigned c=0;c<3;c++){int d=int(a[p+c])-int(b[p+c]);e+=uint64_t(d*d);}}return e;}
static uint64_t sse_all(const Bytes&a,const Bytes&b){uint64_t e=0;for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++){size_t p=(size_t(y)*W+x)*4;for(unsigned c=0;c<3;c++){int d=int(a[p+c])-int(b[p+c]);e+=uint64_t(d*d);}}return e;}
static size_t zstd_bytes(ZSTD_CCtx*ctx,const Bytes&in){Bytes out(ZSTD_compressBound(in.size()));size_t n=ZSTD_compress2(ctx,out.data(),out.size(),in.data(),in.size());if(ZSTD_isError(n))throw std::runtime_error(ZSTD_getErrorName(n));return n;}
static void build_candidate(const Bytes&input,const Bytes&base,Bytes&allcandidate,block_size_descriptor&bsd,unsigned&eligible,unsigned&invalid,std::vector<uint8_t>&valid){
 allcandidate.assign(BYTES,0);valid.assign(NB,0);eligible=invalid=0;const auto&bm=bsd.get_block_mode(CANDIDATE_MODE);assert(bm.get_weight_quant_mode()==QUANT_2);const auto&qat=quant_and_xfer_tables[bm.quant_mode];unsigned qlevels=get_quant_level(bm.get_weight_quant_mode());assert(qlevels==2);unsigned wt[4];for(unsigned i=0;i<4;i++)wt[i]=qat.quant_to_unquant[i];
 const quant_method eq=static_cast<quant_method>(quant_mode_table[3][111-bm.weight_bits]);
 for(unsigned by=0;by<BH;by++)for(unsigned bx=0;bx<BW;bx++){
  const unsigned id=by*BW+bx;const uint8_t*src=base.data()+size_t(id)*16;symbolic_compressed_block old{};physical_to_symbolic(bsd,src,old);
  if(old.block_type!=SYM_BTYPE_NONCONST||old.partition_count!=1||old.color_formats[0]!=FMT_RGB){std::copy_n(src,16,allcandidate.data()+size_t(id)*16);invalid++;continue;}
  bool rgbhdr=false,alphahdr=false;vint4 o0,o1;unpack_color_endpoints(ASTCENC_PRF_LDR,FMT_RGB,old.color_values[0],rgbhdr,alphahdr,o0,o1);if(rgbhdr||alphahdr){std::copy_n(src,16,allcandidate.data()+size_t(id)*16);invalid++;continue;}
  float c0[3]={float(o0.lane<0>())/257.0f,float(o0.lane<1>())/257.0f,float(o0.lane<2>())/257.0f},c1[3]={float(o1.lane<0>())/257.0f,float(o1.lane<1>())/257.0f,float(o1.lane<2>())/257.0f};
  symbolic_compressed_block cand{};cand.block_type=SYM_BTYPE_NONCONST;cand.partition_count=1;cand.partition_index=0;cand.color_formats[0]=FMT_RGB;cand.plane2_component=-1;cand.block_mode=CANDIDATE_MODE;cand.quant_mode=eq;
  auto pe=pack_color_endpoints(vfloat4(c0[0]*257,c0[1]*257,c0[2]*257,65535),vfloat4(c1[0]*257,c1[1]*257,c1[2]*257,65535),vfloat4(0.0f),vfloat4(0.0f),FMT_RGB,cand.color_values[0],eq);if(pe!=FMT_RGB){std::copy_n(src,16,allcandidate.data()+size_t(id)*16);invalid++;continue;}
  vint4 d0,d1;unpack_color_endpoints(ASTCENC_PRF_LDR,FMT_RGB,cand.color_values[0],rgbhdr,alphahdr,d0,d1);uint8_t col0[3]={uint8_t(d0.lane<0>()/257),uint8_t(d0.lane<1>()/257),uint8_t(d0.lane<2>()/257)},col1[3]={uint8_t(d1.lane<0>()/257),uint8_t(d1.lane<1>()/257),uint8_t(d1.lane<2>()/257)};
  for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){size_t p=((by*8+y)*W+bx*8+x)*4;uint64_t best=UINT64_MAX;unsigned bestq=0;for(unsigned q=0;q<qlevels;q++){uint32_t w=wt[q];uint64_t e=0;for(int c=0;c<3;c++){int dec=((unsigned(col0[c])*(64-w)+unsigned(col1[c])*w+32)>>6);int d=int(input[p+c])-dec;e+=uint64_t(d*d);}if(e<best){best=e;bestq=q;}}cand.weights[y*8+x]=qat.quant_to_unquant[bestq];}
  std::array<uint8_t,16> packed{};symbolic_to_physical(bsd,cand,packed.data());symbolic_compressed_block check{};physical_to_symbolic(bsd,packed.data(),check);if(check.block_type!=SYM_BTYPE_NONCONST||check.block_mode!=CANDIDATE_MODE||check.partition_count!=1||check.color_formats[0]!=FMT_RGB){std::copy_n(src,16,allcandidate.data()+size_t(id)*16);invalid++;continue;}std::copy(packed.begin(),packed.end(),allcandidate.begin()+size_t(id)*16);eligible++;valid[id]=1;
 }
}
int main(int argc,char**argv){try{
 if(argc!=5)throw std::runtime_error("usage: gate eye0.rgba eye0.raw.astc eye1.rgba eye1.raw.astc");
 const char*inputs[]={argv[1],argv[3]};const char*blocks[]={argv[2],argv[4]};
 astcenc_config cfg{};if(astcenc_config_init(ASTCENC_PRF_LDR,8,8,1,ASTCENC_PRE_MEDIUM,0,&cfg)!=ASTCENC_SUCCESS)return 2;astcenc_context*ctx=nullptr;if(astcenc_context_alloc(&cfg,1,&ctx)!=ASTCENC_SUCCESS)return 3;auto*bsd=new block_size_descriptor{};init_block_size_descriptor(8,8,1,false,4,1.0f,*bsd);assert(bsd->block_mode_packed_index[CANDIDATE_MODE]!=BLOCK_BAD_BLOCK_MODE);
 std::cout<<"eye,blocks,eligible,invalid,selected,share,baseline_mse,candidate_mse,selected_mse,baseline_zstd_frame,candidate_zstd_frame,selected_zstd_frame,baseline_packet,candidate_packet,selected_packet,baseline_sse,candidate_sse,selected_sse\n";
 for(int eye=0;eye<2;eye++){Bytes input=load(inputs[eye],size_t(W)*H*4),base=load(blocks[eye],BYTES),base_dec(input.size()),cand_dec(input.size()),allcand(BYTES),mixed(BYTES),mix_dec(input.size());decode(ctx,base,base_dec);unsigned eligible=0,invalid=0;std::vector<uint8_t>valid;build_candidate(input,base,allcand,*bsd,eligible,invalid,valid);decode(ctx,allcand,cand_dec);unsigned selected=0;for(unsigned by=0;by<BH;by++)for(unsigned bx=0;bx<BW;bx++){uint64_t eb=sse_block(input,base_dec,bx,by),ec=sse_block(input,cand_dec,bx,by);size_t off=size_t(by*BW+bx)*16;const bool use=valid[by*BW+bx]&&ec*5<=eb*4&&ec<=eb;const Bytes&pick=use?allcand:base;if(&pick==&allcand)selected++;std::copy_n(pick.data()+off,16,mixed.data()+off);}decode(ctx,mixed,mix_dec);uint64_t eb=sse_all(input,base_dec),ec=sse_all(input,cand_dec),em=sse_all(input,mix_dec);ZSTD_CCtx*z=ZSTD_createCCtx();if(!z)throw std::bad_alloc();size_t ps=ZSTD_CCtx_setParameter(z,ZSTD_c_compressionLevel,3);if(ZSTD_isError(ps))throw std::runtime_error(ZSTD_getErrorName(ps));size_t zb=zstd_bytes(z,base),zc=zstd_bytes(z,allcand),zm=zstd_bytes(z,mixed);ZSTD_freeCCtx(z);double denom=double(W)*H*3;std::cout<<eye<<","<<NB<<","<<eligible<<","<<invalid<<","<<selected<<","<<double(selected)/NB<<","<<eb/denom<<","<<ec/denom<<","<<em/denom<<","<<zb<<","<<zc<<","<<zm<<","<<zb+24<<","<<zc+24<<","<<zm+24<<","<<eb<<","<<ec<<","<<em<<"\n";}
 astcenc_context_free(ctx);delete bsd;return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\\n";return 1;}}
