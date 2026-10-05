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
static constexpr unsigned CANDIDATE_MODE=0x108;
using Bytes=std::vector<uint8_t>;
static Bytes load(const char*p,size_t n){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error(std::string("open ")+p);Bytes b(n);f.read((char*)b.data(),n);if(size_t(f.gcount())!=n)throw std::runtime_error(std::string("short read ")+p);if(f.peek()!=std::char_traits<char>::eof())throw std::runtime_error(std::string("oversized input ")+p);return b;}
static void decode(astcenc_context*ctx,const Bytes&astc,Bytes&rgba){void*sl[]={rgba.data()};astcenc_image im{W,H,1,ASTCENC_TYPE_U8,sl};astcenc_swizzle sw{ASTCENC_SWZ_R,ASTCENC_SWZ_G,ASTCENC_SWZ_B,ASTCENC_SWZ_A};auto e=astcenc_decompress_image(ctx,astc.data(),astc.size(),&im,&sw,0);if(e!=ASTCENC_SUCCESS)throw std::runtime_error(astcenc_get_error_string(e));}
static inline uint64_t sse_block(const Bytes&a,const Bytes&b,unsigned bx,unsigned by){uint64_t e=0;for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){size_t p=((by*8+y)*W+bx*8+x)*4;for(unsigned c=0;c<3;c++){int d=int(a[p+c])-int(b[p+c]);e+=uint64_t(d*d);}}return e;}
static uint64_t sse_all(const Bytes&a,const Bytes&b){uint64_t e=0;for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++){size_t p=(size_t(y)*W+x)*4;for(unsigned c=0;c<3;c++){int d=int(a[p+c])-int(b[p+c]);e+=uint64_t(d*d);}}return e;}
static size_t zstd_bytes(ZSTD_CCtx*ctx,const Bytes&in){Bytes out(ZSTD_compressBound(in.size()));size_t n=ZSTD_compress2(ctx,out.data(),out.size(),in.data(),in.size());if(ZSTD_isError(n))throw std::runtime_error(ZSTD_getErrorName(n));return n;}
static bool pseudoinverse(const decimation_info&di,double pinv[36][64]){
 double a[36][72]{};double design[64][36]{};
 for(unsigned p=0;p<64;p++)for(unsigned j=0;j<di.texel_weight_count[p];j++)design[p][di.texel_weights_tr[j][p]]=di.texel_weight_contribs_float_tr[j][p];
 for(unsigned i=0;i<36;i++)for(unsigned j=0;j<36;j++)for(unsigned p=0;p<64;p++)a[i][j]+=design[p][i]*design[p][j];
 for(unsigned i=0;i<36;i++)a[i][36+i]=1;
 for(unsigned c=0;c<36;c++){unsigned pivot=c;for(unsigned r=c+1;r<36;r++)if(std::abs(a[r][c])>std::abs(a[pivot][c]))pivot=r;if(std::abs(a[pivot][c])<1e-12)return false;for(unsigned k=0;k<72;k++)std::swap(a[c][k],a[pivot][k]);double d=a[c][c];for(unsigned k=0;k<72;k++)a[c][k]/=d;for(unsigned r=0;r<36;r++)if(r!=c){double f=a[r][c];for(unsigned k=0;k<72;k++)a[r][k]-=f*a[c][k];}}
 for(unsigned i=0;i<36;i++)for(unsigned p=0;p<64;p++)for(unsigned j=0;j<36;j++)pinv[i][p]+=a[i][36+j]*design[p][j];
 return true;
}
static void build_candidate(const Bytes&input,const Bytes&base,Bytes&allcandidate,block_size_descriptor&bsd,const double pinv[36][64],unsigned&eligible,unsigned&invalid,unsigned&direct,unsigned&delta,std::vector<uint8_t>&valid){
 allcandidate.assign(BYTES,0);valid.assign(NB,0);eligible=invalid=direct=delta=0;const auto&bm=bsd.get_block_mode(CANDIDATE_MODE);const auto&di=bsd.get_decimation_info(bm.decimation_mode);if(di.weight_count!=36||di.weight_x!=6||di.weight_y!=6||bm.get_weight_quant_mode()!=QUANT_4)throw std::runtime_error("mode 0x108 is not 6x6 Q4");
 const auto&qat=quant_and_xfer_tables[bm.quant_mode];unsigned qlevels=get_quant_level(bm.get_weight_quant_mode());if(qlevels!=4)throw std::runtime_error("Q4 quantizer level mismatch");
 const quant_method eq=static_cast<quant_method>(quant_mode_table[3][111-bm.weight_bits]);if(get_quant_level(eq)!=80)throw std::runtime_error("expected Q80 endpoint quantization");
 for(unsigned by=0;by<BH;by++)for(unsigned bx=0;bx<BW;bx++){
  const unsigned id=by*BW+bx;const uint8_t*src=base.data()+size_t(id)*16;symbolic_compressed_block old{};physical_to_symbolic(bsd,src,old);
  if(old.block_type!=SYM_BTYPE_NONCONST){std::copy_n(src,16,allcandidate.data()+size_t(id)*16);invalid++;continue;}if(old.partition_count!=1){std::copy_n(src,16,allcandidate.data()+size_t(id)*16);invalid++;continue;}if(old.color_formats[0]!=FMT_RGB){std::copy_n(src,16,allcandidate.data()+size_t(id)*16);invalid++;continue;}
  bool rgbhdr=false,alphahdr=false;vint4 o0,o1;unpack_color_endpoints(ASTCENC_PRF_LDR,FMT_RGB,old.color_values[0],rgbhdr,alphahdr,o0,o1);if(rgbhdr||alphahdr){std::copy_n(src,16,allcandidate.data()+size_t(id)*16);invalid++;continue;}
  float c0[3]={float(o0.lane<0>())/257.0f,float(o0.lane<1>())/257.0f,float(o0.lane<2>())/257.0f},c1[3]={float(o1.lane<0>())/257.0f,float(o1.lane<1>())/257.0f,float(o1.lane<2>())/257.0f};
  symbolic_compressed_block cand{};cand.block_type=SYM_BTYPE_NONCONST;cand.partition_count=1;cand.partition_index=0;cand.color_formats[0]=FMT_RGB;cand.plane2_component=-1;cand.block_mode=CANDIDATE_MODE;cand.quant_mode=eq;
  auto pe=pack_color_endpoints(vfloat4(c0[0]*257,c0[1]*257,c0[2]*257,65535),vfloat4(c1[0]*257,c1[1]*257,c1[2]*257,65535),vfloat4(0.0f),vfloat4(0.0f),FMT_RGB,cand.color_values[0],eq);if(pe!=FMT_RGB&&pe!=FMT_RGB_DELTA){std::copy_n(src,16,allcandidate.data()+size_t(id)*16);invalid++;continue;}cand.color_formats[0]=pe;
  vint4 d0,d1;unpack_color_endpoints(ASTCENC_PRF_LDR,cand.color_formats[0],cand.color_values[0],rgbhdr,alphahdr,d0,d1);double ep0[3]={double(d0.lane<0>()/257),double(d0.lane<1>()/257),double(d0.lane<2>()/257)},ep1[3]={double(d1.lane<0>()/257),double(d1.lane<1>()/257),double(d1.lane<2>()/257)};double axis[3],den=0;for(int c=0;c<3;c++){axis[c]=ep1[c]-ep0[c];den+=axis[c]*axis[c];}if(den<1e-8){for(unsigned w=0;w<36;w++)cand.weights[w]=qat.quant_to_unquant[0];}else{double target[64];for(unsigned p=0;p<64;p++){unsigned x=p&7,y=p>>3;size_t ix=((by*8+y)*W+bx*8+x)*4;double dot=0;for(int c=0;c<3;c++)dot+=(double(input[ix+c])-ep0[c])*axis[c];target[p]=std::clamp(dot/den,0.0,1.0);}
  for(unsigned w=0;w<36;w++){double fitted=0;for(unsigned p=0;p<64;p++)fitted+=pinv[w][p]*target[p];fitted=std::clamp(fitted,0.0,1.0);double best=1e30;unsigned bestq=0;for(unsigned q=0;q<qlevels;q++){double e=std::abs(fitted*64.0-double(qat.quant_to_unquant[q]));if(e<best){best=e;bestq=q;}}cand.weights[w]=qat.quant_to_unquant[bestq];}}
  std::array<uint8_t,16> packed{};symbolic_to_physical(bsd,cand,packed.data());symbolic_compressed_block check{};physical_to_symbolic(bsd,packed.data(),check);if(check.block_type!=SYM_BTYPE_NONCONST||check.block_mode!=CANDIDATE_MODE||check.partition_count!=1||check.color_formats[0]!=cand.color_formats[0]||get_quant_level(check.quant_mode)!=80){std::copy_n(src,16,allcandidate.data()+size_t(id)*16);invalid++;continue;}std::copy(packed.begin(),packed.end(),allcandidate.begin()+size_t(id)*16);eligible++;valid[id]=1;if(cand.color_formats[0]==FMT_RGB)direct++;else if(cand.color_formats[0]==FMT_RGB_DELTA)delta++;
 }
}
int main(int argc,char**argv){try{
 if(argc!=5)throw std::runtime_error("usage: gate eye0.rgba eye0.raw.astc eye1.rgba eye1.raw.astc");
 const char*inputs[]={argv[1],argv[3]};const char*blocks[]={argv[2],argv[4]};
 astcenc_config cfg{};if(astcenc_config_init(ASTCENC_PRF_LDR,8,8,1,ASTCENC_PRE_MEDIUM,0,&cfg)!=ASTCENC_SUCCESS)return 2;astcenc_context*ctx=nullptr;if(astcenc_context_alloc(&cfg,1,&ctx)!=ASTCENC_SUCCESS)return 3;auto*bsd=new block_size_descriptor{};init_block_size_descriptor(8,8,1,false,4,1.0f,*bsd);assert(bsd->block_mode_packed_index[CANDIDATE_MODE]!=BLOCK_BAD_BLOCK_MODE);double pinv[36][64]{};const auto&modeinfo=bsd->get_decimation_info(bsd->get_block_mode(CANDIDATE_MODE).decimation_mode);if(!pseudoinverse(modeinfo,pinv))throw std::runtime_error("6x6 decimation matrix singular");
 std::cout<<"eye,blocks,eligible,invalid,q4_cem8,q4_cem9,selected,share,baseline_mse,q4_mse,selected_mse,baseline_zstd_frame,q4_zstd_frame,selected_zstd_frame,baseline_packet,q4_packet,selected_packet,baseline_sse,q4_sse,selected_sse\n";
 for(int eye=0;eye<2;eye++){Bytes input=load(inputs[eye],size_t(W)*H*4),base=load(blocks[eye],BYTES),base_dec(input.size()),cand_dec(input.size()),allcand(BYTES),mixed(BYTES),mix_dec(input.size());decode(ctx,base,base_dec);unsigned eligible=0,invalid=0,direct=0,delta=0;std::vector<uint8_t>valid;build_candidate(input,base,allcand,*bsd,pinv,eligible,invalid,direct,delta,valid);decode(ctx,allcand,cand_dec);unsigned selected=0;for(unsigned by=0;by<BH;by++)for(unsigned bx=0;bx<BW;bx++){uint64_t eb=sse_block(input,base_dec,bx,by),ec=sse_block(input,cand_dec,bx,by);size_t off=size_t(by*BW+bx)*16;const bool use=valid[by*BW+bx]&&ec*5<=eb*4&&ec<eb;const Bytes&pick=use?allcand:base;if(&pick==&allcand)selected++;std::copy_n(pick.data()+off,16,mixed.data()+off);}decode(ctx,mixed,mix_dec);uint64_t eb=sse_all(input,base_dec),ec=sse_all(input,cand_dec),em=sse_all(input,mix_dec);ZSTD_CCtx*z=ZSTD_createCCtx();if(!z)throw std::bad_alloc();size_t ps=ZSTD_CCtx_setParameter(z,ZSTD_c_compressionLevel,3);if(ZSTD_isError(ps))throw std::runtime_error(ZSTD_getErrorName(ps));size_t zb=zstd_bytes(z,base),zc=zstd_bytes(z,allcand),zm=zstd_bytes(z,mixed);ZSTD_freeCCtx(z);double denom=double(W)*H*3;std::cout<<eye<<","<<NB<<","<<eligible<<","<<invalid<<","<<direct<<","<<delta<<","<<selected<<","<<double(selected)/NB<<","<<eb/denom<<","<<ec/denom<<","<<em/denom<<","<<zb<<","<<zc<<","<<zm<<","<<zb+24<<","<<zc+24<<","<<zm+24<<","<<eb<<","<<ec<<","<<em<<"\n";}
 astcenc_context_free(ctx);delete bsd;return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\\n";return 1;}}
