#include "lz4.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using Bytes=std::vector<char>;
using Clock=std::chrono::steady_clock;
struct Chunk {int raw; Bytes data; bool compressed;};
static Bytes read(const char *p){std::ifstream f(p,std::ios::binary);if(!f)std::abort();return Bytes(std::istreambuf_iterator<char>(f),{});}
static double pct(std::vector<double> a,double q){std::sort(a.begin(),a.end());return a[std::min(a.size()-1,size_t(q*a.size()))];}
int main(int argc,char**argv){
 if(argc<4)return 2;
 bool encode=std::string(argv[1])=="encode";
 auto input=read(argv[2]);std::vector<Chunk> chunks;Bytes original;
 int chunkSize=atoi(argv[3]);
 if(encode){
 original=input;
 for(size_t o=0;o<input.size();o+=chunkSize){int n=std::min(size_t(chunkSize),input.size()-o);Chunk c{n,Bytes(size_t(LZ4_compressBound(n))),true};
 int z=LZ4_compress_default(input.data()+o,c.data.data(),n,c.data.size());if(!z)abort();
 if(z>=n){c.data.assign(input.begin()+o,input.begin()+o+n);c.compressed=false;}else c.data.resize(z);
 chunks.push_back(std::move(c));}
 std::ofstream f(std::string(argv[2])+"."+argv[3]+".lz4b",std::ios::binary);
 for(auto &c:chunks){uint32_t h[3]={uint32_t(c.raw),uint32_t(c.data.size()),uint32_t(c.compressed)};f.write((char*)h,12);f.write(c.data.data(),c.data.size());}if(!f)abort();
 }else{
 size_t o=0;while(o<input.size()){if(input.size()-o<12)abort();uint32_t h[3];memcpy(h,input.data()+o,12);o+=12;if(!h[0]||h[0]>10000000||h[1]>input.size()-o||h[2]>1)abort();chunks.push_back({int(h[0]),Bytes(input.begin()+o,input.begin()+o+h[1]),bool(h[2])});o+=h[1];}
 std::string path=argv[2];auto suffix=path.rfind(".",path.size()-6);original=read(path.substr(0,suffix).c_str());
 }
 size_t raw=0,packed=0;for(auto&c:chunks){raw+=c.raw;packed+=12+c.data.size();}if(raw!=original.size())abort();
 Bytes out(raw);size_t offset=0;for(auto &c:chunks){if(c.compressed){if(LZ4_decompress_safe(c.data.data(),out.data()+offset,c.data.size(),c.raw)!=c.raw)abort();}else memcpy(out.data()+offset,c.data.data(),c.raw);offset+=c.raw;}if(out!=original)abort();
 Bytes scratch(LZ4_compressBound(raw));std::vector<double> times;
 for(int k=0;k<120;k++){auto start=Clock::now();size_t off=0;for(auto &c:chunks){if(encode){if(!LZ4_compress_default(original.data()+off,scratch.data(),c.raw,scratch.size()))abort();}else if(c.compressed){if(LZ4_decompress_safe(c.data.data(),out.data()+off,c.data.size(),c.raw)!=c.raw)abort();}else memcpy(out.data()+off,c.data.data(),c.raw);off+=c.raw;}double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();if(k>=20)times.push_back(ms);}
 if(!encode && out!=original)abort();
 printf("%s,%s,%d,%zu,%zu,%zu,%.6f,%.6f\n",encode?"encode":"decode",argv[2],chunkSize,raw,packed,chunks.size(),pct(times,.50),pct(times,.95));
}
