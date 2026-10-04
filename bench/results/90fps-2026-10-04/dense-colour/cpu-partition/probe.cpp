#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>
#include "/run/media/nerdrx/Lex/claude/nx-scratch/nx-xuastc-20261003/basis_universal/transcoder/basisu_transcoder.h"
#include "/run/media/nerdrx/Lex/claude/nx-scratch/nx-xuastc-20261003/basis_universal/transcoder/basisu_containers_impl.h"
#define BASISU_ASTC_HELPERS_IMPLEMENTATION
#include "/run/media/nerdrx/Lex/claude/nx-scratch/nx-xuastc-20261003/basis_universal/transcoder/basisu_astc_helpers.h"

using namespace astc_helpers;
using RGB=std::array<double,3>;
using Px=std::array<uint8_t,4>;
static void put(std::vector<uint8_t>& v,uint32_t bit,uint32_t n,uint32_t x){for(uint32_t i=0;i<n;i++)if(x&(1u<<i))v[(bit+i)>>3]|=uint8_t(1u<<((bit+i)&7));}
static std::array<double,3> mul(const double a[3][3],std::array<double,3> v){return {a[0][0]*v[0]+a[0][1]*v[1]+a[0][2]*v[2],a[1][0]*v[0]+a[1][1]*v[1]+a[1][2]*v[2],a[2][0]*v[0]+a[2][1]*v[1]+a[2][2]*v[2]};}
struct Fit{RGB lo{},hi{};};
static Fit fit_line(const std::array<Px,64>& p,const std::array<uint8_t,64>& part,int id){
 RGB mean{};int n=0;for(int i=0;i<64;i++)if(part[i]==id){for(int c=0;c<3;c++)mean[c]+=p[i][c];n++;}for(auto&x:mean)x/=std::max(n,1);
 double c[3][3]{};for(int i=0;i<64;i++)if(part[i]==id)for(int a=0;a<3;a++)for(int b=0;b<3;b++)c[a][b]+=(p[i][a]-mean[a])*(p[i][b]-mean[b]);
 RGB axis={1,0,0};for(int k=0;k<10;k++){auto v=mul(c,axis);double l=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(l<1e-12)break;for(int a=0;a<3;a++)axis[a]=v[a]/l;}
 double mn=1e30,mx=-1e30;Fit f;for(int i=0;i<64;i++)if(part[i]==id){double t=0;for(int a=0;a<3;a++)t+=(p[i][a]-mean[a])*axis[a];if(t<mn){mn=t;for(int a=0;a<3;a++)f.lo[a]=p[i][a];}if(t>mx){mx=t;for(int a=0;a<3;a++)f.hi[a]=p[i][a];}}
 RGB q0{},q1{};for(int a=0;a<3;a++){q0[a]=dequant_bise_endpoint(find_nearest_bise_endpoint(int(std::lround(f.lo[a])),BISE_16_LEVELS),BISE_16_LEVELS);q1[a]=dequant_bise_endpoint(find_nearest_bise_endpoint(int(std::lround(f.hi[a])),BISE_16_LEVELS),BISE_16_LEVELS);}
 if(q0[0]+q0[1]+q0[2]>q1[0]+q1[1]+q1[2])std::swap(q0,q1);f.lo=q0;f.hi=q1;return f;
}
static bool solve(double a[16][17],double x[16]){for(int c=0;c<16;c++){int p=c;for(int r=c+1;r<16;r++)if(std::abs(a[r][c])>std::abs(a[p][c]))p=r;if(std::abs(a[p][c])<1e-8)return false;for(int k=c;k<17;k++)std::swap(a[c][k],a[p][k]);double z=a[c][c];for(int k=c;k<17;k++)a[c][k]/=z;for(int r=0;r<16;r++)if(r!=c){z=a[r][c];for(int k=c;k<17;k++)a[r][k]-=z*a[c][k];}}for(int i=0;i<16;i++)x[i]=a[i][16];return true;}
struct Candidate{double mse=1e99;uint32_t seed=0;astc_block bits{};log_astc_block log{};};
static Candidate encode(const std::array<Px,64>& pix){
 std::array<weighted_sample,64> up{};compute_upsample_weights(8,8,4,4,up.data());Candidate best;
 for(uint32_t seed=0;seed<16;seed++){
  std::array<uint8_t,64> part{};int counts[2]{};for(int i=0;i<64;i++){part[i]=uint8_t(compute_texel_partition(seed,i%8,i/8,0,2,false));counts[part[i]]++;}if(counts[0]<8||counts[1]<8)continue;
  std::array<Fit,2> fs{fit_line(pix,part,0),fit_line(pix,part,1)};std::array<RGB,2> axis{};double den[2]{};for(int s=0;s<2;s++){for(int c=0;c<3;c++)axis[s][c]=fs[s].hi[c]-fs[s].lo[c];for(int c=0;c<3;c++)den[s]+=axis[s][c]*axis[s][c];}
  double normal[16][17]{};
  for(int i=0;i<64;i++){auto&u=up[i];int sx=u.m_src_x,sy=u.m_src_y;int ix[4]={sx,sx+1,sx,sx+1},iy[4]={sy,sy,sy+1,sy+1};double coef[4]={u.m_weights[0][0]/16.0,u.m_weights[0][1]/16.0,u.m_weights[1][0]/16.0,u.m_weights[1][1]/16.0};int s=part[i];double target=0;if(den[s]>1e-8)for(int c=0;c<3;c++)target+=(pix[i][c]-fs[s].lo[c])*axis[s][c]/den[s];target=std::clamp(target,0.0,1.0);
   for(int j=0;j<4;j++)for(int k=0;k<4;k++)normal[iy[j]*4+ix[j]][iy[k]*4+ix[k]]+=coef[j]*coef[k];for(int j=0;j<4;j++)normal[iy[j]*4+ix[j]][16]+=coef[j]*target;
  }
  double wt[16]{};if(!solve(normal,wt))continue;log_astc_block b{};b.m_num_partitions=2;b.m_partition_id=seed;b.m_grid_width=4;b.m_grid_height=4;b.m_weight_ise_range=BISE_8_LEVELS;b.m_endpoint_ise_range=BISE_16_LEVELS;b.m_color_endpoint_modes[0]=b.m_color_endpoint_modes[1]=CEM_LDR_RGB_DIRECT;
  for(int s=0;s<2;s++)for(int c=0;c<3;c++){b.m_endpoints[s*6+2*c]=find_nearest_bise_endpoint(int(std::lround(fs[s].lo[c])),BISE_16_LEVELS);b.m_endpoints[s*6+2*c+1]=find_nearest_bise_endpoint(int(std::lround(fs[s].hi[c])),BISE_16_LEVELS);}
  for(int i=0;i<16;i++)b.m_weights[i]=find_nearest_bise_weight(int(std::lround(std::clamp(wt[i],0.0,1.0)*64)),BISE_8_LEVELS);
  astc_block phys{};int expected=-1;if(!pack_astc_block(phys,b,&expected))continue;
  Px out[64];if(!decode_block(b,out,8,8,cDecodeModeLDR8))continue;double e=0;for(int i=0;i<64;i++)for(int c=0;c<3;c++){double d=double(pix[i][c])-out[i][c];e+=d*d;}if(e<best.mse){best.mse=e;best.seed=seed;best.bits=phys;best.log=b;}
 }
 return best;
}
int main(int argc,char**argv){if(argc!=4){std::cerr<<"usage: probe input-rgba w h\n";return 2;}int w=std::atoi(argv[2]),h=std::atoi(argv[3]);std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t>src((std::istreambuf_iterator<char>(f)),{});if(src.size()!=size_t(w*h*4)||w%8||h%8)return 3;init_tables();std::vector<uint8_t> out(size_t(w/8)*(h/8)*16);double sum=0;uint64_t n=0;Candidate chosen;
 for(int by=0;by<h;by+=8)for(int bx=0;bx<w;bx+=8){std::array<Px,64> p{};for(int y=0;y<8;y++)for(int x=0;x<8;x++)std::copy_n(src.data()+((by+y)*w+bx+x)*4,4,p[y*8+x].begin());Candidate c=encode(p);if(c.mse>1e98){std::cerr<<"no packed partition\n";return 4;}sum+=c.mse;n+=192;if(bx==0&&by==0)chosen=c;std::copy_n(c.bits.m_vals,4,reinterpret_cast<uint32_t*>(out.data()+((by/8)*(w/8)+bx/8)*16));}
 std::ofstream o("candidate.blocks",std::ios::binary);o.write((char*)out.data(),out.size());
 std::ofstream lut("partition-lut.txt"); for(uint32_t seed=0;seed<16;seed++){lut<<"seed "<<seed<<"\n";for(int y=0;y<8;y++){for(int x=0;x<8;x++)lut<<compute_texel_partition(seed,x,y,0,2,false);lut<<"\n";}}
 std::cout<<"mse="<<sum/n<<" psnr="<<10*std::log10(255.0*255.0/(sum/n))<<" first-seed="<<chosen.seed<<" header="<<std::hex<<chosen.bits.m_vals[0]<<std::dec<<" ep-range="<<int(chosen.log.m_endpoint_ise_range)<<" weight-range="<<int(chosen.log.m_weight_ise_range)<<" raw-bytes="<<out.size()<<"\n";
 for(int y=0;y<8;y++){for(int x=0;x<8;x++)std::cout<<compute_texel_partition(chosen.seed,x,y,0,2,false);std::cout<<"\n";}for(auto v:chosen.bits.m_vals)std::cout<<std::hex<<v<<" ";std::cout<<"\n";return 0;}
