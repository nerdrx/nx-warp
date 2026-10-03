#include "basis_universal/encoder/3rdparty/android_astc_decomp.cpp"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
int main(int argc,char**argv){
 if(argc!=3)return 2; std::ifstream in(argv[1],std::ios::binary); std::vector<uint8_t>b((std::istreambuf_iterator<char>(in)),{});
 const uint8_t m[4]={0x13,0xAB,0xA1,0x5C}; if(b.size()<16||!std::equal(m,m+4,b.begin()))return 3;
 int bw=b[4],bh=b[5],w=b[7]|(b[8]<<8)|(b[9]<<16),h=b[10]|(b[11]<<8)|(b[12]<<16),d=b[13]|(b[14]<<8)|(b[15]<<16);
 if(!bw||!bh||d!=1)return 4; size_t nx=(w+bw-1)/bw,ny=(h+bh-1)/bh; if(b.size()!=16+nx*ny*16)return 5;
 std::vector<uint8_t>img((size_t)w*h*4),blk((size_t)bw*bh*4); size_t k=16;
 for(size_t by=0;by<ny;by++)for(size_t bx=0;bx<nx;bx++,k+=16){if(!basisu_astc::astc::decompress_ldr(blk.data(),b.data()+k,true,bw,bh,false)){std::cerr<<"bad block "<<bx<<","<<by<<"\n";return 6;}
  for(int y=0;y<bh&&by*bh+y<(size_t)h;y++){size_t x=bx*bw;if(x<(size_t)w){size_t n=std::min((size_t)bw,(size_t)w-x);std::copy_n(blk.data()+(size_t)y*bw*4,n*4,img.data()+((by*bh+y)*(size_t)w+x)*4);}}
 }
 std::ofstream out(argv[2],std::ios::binary);out.write((char*)img.data(),img.size());if(!out)return 7;std::cout<<w<<"x"<<h<<" RGBA8\n";
}
