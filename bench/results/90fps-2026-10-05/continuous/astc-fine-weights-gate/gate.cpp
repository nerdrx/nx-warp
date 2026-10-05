#include "astcenc_internal.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using RGB = std::array<float, 3>;
using Image = std::array<RGB, 64>;
struct Trial { const char *name; unsigned mode; };
static constexpr Trial trials[]={{"5x5-Q8",0x0f3},{"6x6-Q4",0x108},{"8x8-Q2",0x544}};
static const char *AST="/run/media/nerdrx/Lex/claude/nx-scratch/wivrn-hybrid-client-build/_deps/libktx-src/external/astc-encoder/Source";

static RGB pixel(const Image &im,int x,int y) { return im[y*8+x]; }
static float lum(const RGB &c) { return 0.2126f*c[0]+0.7152f*c[1]+0.0722f*c[2]; }
static Image ramp() { Image im; for(int y=0;y<8;y++)for(int x=0;x<8;x++){float v=255.0f*x/7;im[y*8+x]={v,v,v};}return im; }
static Image edge(int edge_x) { Image im;for(int y=0;y<8;y++)for(int x=0;x<8;x++){float v=x<edge_x?0:255;im[y*8+x]={v,v,v};}return im; }
static Image blobs() { Image im; const RGB c[4]={{255,20,10},{10,255,20},{20,10,255},{255,240,10}};for(int y=0;y<8;y++)for(int x=0;x<8;x++)im[y*8+x]=c[(y>=4)*2+(x>=4)];return im; }

static bool solve(double a[64][65],int n,double *x) {
 for(int c=0;c<n;c++){int p=c;for(int r=c+1;r<n;r++)if(std::abs(a[r][c])>std::abs(a[p][c]))p=r;if(std::abs(a[p][c])<1e-10)return false;
  for(int k=c;k<=n;k++)std::swap(a[c][k],a[p][k]);double d=a[c][c];for(int k=c;k<=n;k++)a[c][k]/=d;
  for(int r=0;r<n;r++)if(r!=c){double f=a[r][c];for(int k=c;k<=n;k++)a[r][k]-=f*a[c][k];}}
 for(int i=0;i<n;i++)x[i]=a[i][n];return true;
}
static std::pair<RGB,RGB> endpoints(const Image &im) {
 float best=-1;RGB a{},b{};for(int i=0;i<64;i++)for(int j=i+1;j<64;j++){float d=0;for(int c=0;c<3;c++){float t=im[i][c]-im[j][c];d+=t*t;}if(d>best){best=d;a=im[i];b=im[j];}}
 return {a,b};
}
static std::array<uint8_t,16> encode(const Image &im,const Trial &t,const block_size_descriptor &bsd) {
 const auto &bm=bsd.get_block_mode(t.mode);const auto &di=bsd.get_decimation_info(bm.decimation_mode);
 assert(di.weight_count== (t.mode==0x0f3?25:t.mode==0x108?36:64));
 auto [e0,e1]=endpoints(im);if(e0[0]+e0[1]+e0[2]>e1[0]+e1[1]+e1[2])std::swap(e0,e1);RGB axis;float den=0;for(int c=0;c<3;c++){axis[c]=e1[c]-e0[c];den+=axis[c]*axis[c];}den=std::max(den,1e-8f);
 double normal[64][65]{};double targets[64]{};
 for(int p=0;p<64;p++){RGB src=im[p];double q=0;for(int c=0;c<3;c++)q+=(src[c]-e0[c])*axis[c];targets[p]=std::clamp(q/den,0.0,1.0);
  for(int j=0;j<di.texel_weight_count[p];j++){int wi=di.texel_weights_tr[j][p];double cj=di.texel_weight_contribs_float_tr[j][p];normal[wi][di.weight_count]+=cj*targets[p];for(int k=0;k<di.texel_weight_count[p];k++){int wk=di.texel_weights_tr[k][p];normal[wi][wk]+=cj*di.texel_weight_contribs_float_tr[k][p];}}}
 double fitted[64]{};assert(solve(normal,di.weight_count,fitted));
 symbolic_compressed_block scb{};scb.block_type=SYM_BTYPE_NONCONST;scb.partition_count=1;scb.partition_index=0;scb.color_formats[0]=FMT_RGB;scb.plane2_component=-1;scb.block_mode=static_cast<uint16_t>(t.mode);scb.quant_mode=static_cast<quant_method>(quant_mode_table[3][111-bm.weight_bits]);assert(scb.quant_mode>=QUANT_6);
 assert(pack_color_endpoints(vfloat4(e0[0]*257,e0[1]*257,e0[2]*257,65535),vfloat4(e1[0]*257,e1[1]*257,e1[2]*257,65535),vfloat4(0.0f),vfloat4(0.0f),FMT_RGB,scb.color_values[0],scb.quant_mode)==FMT_RGB);
 const auto &qat=quant_and_xfer_tables[bm.quant_mode];unsigned levels=get_quant_level(bm.get_weight_quant_mode());
 for(int w=0;w<di.weight_count;w++){double bestd=1e9;uint8_t bestv=0;for(unsigned q=0;q<levels;q++){uint8_t v=qat.quant_to_unquant[q];double d=std::abs(std::clamp(fitted[w],0.0,1.0)*64-v);if(d<bestd){bestd=d;bestv=v;}}scb.weights[w]=bestv;}
 std::array<uint8_t,16> block{};symbolic_to_physical(bsd,scb,block.data());symbolic_compressed_block parsed{};physical_to_symbolic(bsd,block.data(),parsed);assert(parsed.block_type==SYM_BTYPE_NONCONST&&parsed.block_mode==t.mode&&parsed.partition_count==1&&parsed.color_formats[0]==FMT_RGB);
 return block;
}
static Image decode(astcenc_context *ctx,const std::array<uint8_t,16> &block) {
 Image image{};std::array<uint8_t,8*8*4> rgba{};void *slices[1]={rgba.data()};astcenc_image out{8,8,1,ASTCENC_TYPE_U8,slices};astcenc_swizzle swz{ASTCENC_SWZ_R,ASTCENC_SWZ_G,ASTCENC_SWZ_B,ASTCENC_SWZ_1};
 auto err=astcenc_decompress_image(ctx,block.data(),block.size(),&out,&swz,0);if(err!=ASTCENC_SUCCESS)throw std::runtime_error(astcenc_get_error_string(err));
 for(int i=0;i<64;i++)for(int c=0;c<3;c++)image[i][c]=rgba[i*4+c];return image;
}
static double mse(const Image&a,const Image&b){double e=0;for(int i=0;i<64;i++)for(int c=0;c<3;c++){double d=a[i][c]-b[i][c];e+=d*d;}return e/(64*3);}
static double edgewidth(const Image&im){float low=lum(im[0]),high=lum(im[7]);if(low>high)std::swap(low,high);float lo=low+.1f*(high-low),hi=low+.9f*(high-low);int first=8,last=-1;for(int x=0;x<8;x++){float v=lum(im[4*8+x]);if(v>=lo&&v<=hi){first=std::min(first,x);last=x;}}return last<0?0.0:double(last-first+1);}
static void ppm(const std::string &path,const std::vector<std::vector<Image>>&panels){int scale=24;int w=scale*8*3,h=scale*8*int(panels.size());std::ofstream f(path,std::ios::binary);f<<"P6\n"<<w<<" "<<h<<"\n255\n";for(const auto&row:panels)for(int y=0;y<8;y++)for(int sy=0;sy<scale;sy++)for(int m=0;m<3;m++)for(int x=0;x<8;x++)for(int sx=0;sx<scale;sx++){auto c=row[m][y*8+x];for(float v:c)f.put(char(std::clamp(int(std::lround(v)),0,255)));}}
int main(){astcenc_config config{};if(astcenc_config_init(ASTCENC_PRF_LDR,8,8,1,ASTCENC_PRE_MEDIUM,0,&config)!=ASTCENC_SUCCESS)return 2;astcenc_context*ctx=nullptr;if(astcenc_context_alloc(&config,1,&ctx)!=ASTCENC_SUCCESS)return 3;
 auto *bsd=new block_size_descriptor{};init_block_size_descriptor(8,8,1,false,4,1.0f,*bsd);for(auto t:trials)assert(bsd->block_mode_packed_index[t.mode]!=BLOCK_BAD_BLOCK_MODE);
 std::vector<std::pair<std::string,Image>> inputs{{"ramp",ramp()},{"edge3",edge(3)},{"edge4",edge(4)},{"edge5",edge(5)},{"blobs",blobs()}};std::vector<std::vector<Image>> panels;std::cout<<"pattern,mode,mode_id,weight_grid,weight_levels,endpoint_quant,mse,edge_width_px,bytes\n";
 std::vector<std::vector<Image>> all;for(const auto&[name,src]:inputs){std::vector<Image> row;for(auto t:trials){auto block=encode(src,t,*bsd);auto out=decode(ctx,block);symbolic_compressed_block parsed{};physical_to_symbolic(*bsd,block.data(),parsed);auto bm=bsd->get_block_mode(parsed.block_mode);std::cout<<name<<","<<t.name<<",0x"<<std::hex<<t.mode<<std::dec<<","<<int(bsd->get_decimation_info(bm.decimation_mode).weight_x)<<"x"<<int(bsd->get_decimation_info(bm.decimation_mode).weight_y)<<","<<get_quant_level(bm.get_weight_quant_mode())<<","<<get_quant_level(parsed.quant_mode)<<","<<mse(src,out)<<","<<(name.rfind("edge",0)==0?edgewidth(out):0)<<",16\n";row.push_back(out);}all.push_back(row);}ppm("comparison.ppm",all);
 // Single-texel edge shift proxy: compare decoded change to the true source change.
 for(int pair=0;pair<2;pair++){const Image&a=inputs[1+pair].second;const Image&b=inputs[2+pair].second;std::cout<<"phase_pair,"<<inputs[1+pair].first<<"-"<<inputs[2+pair].first<<",source_mse="<<mse(a,b);for(int m=0;m<3;m++)std::cout<<","<<trials[m].name<<"_decoded_mse="<<mse(all[1+pair][m],all[2+pair][m]);std::cout<<"\n";}
 astcenc_context_free(ctx);delete bsd;}
