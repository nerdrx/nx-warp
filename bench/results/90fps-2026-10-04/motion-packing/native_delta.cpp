#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <zstd.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr int W = 240, H = 135, BYTES = 16;
using Block = std::array<uint8_t, BYTES>;
struct Image { int w, h, eye_w=0; std::vector<Block> b; };
struct Encoded { std::vector<uint8_t> selectors, residual; };
struct Result { double enc50, enc95, dec50, dec95, raw50, raw95, rawdec50, rawdec95; size_t raw, delta; };

Image load_astc(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  if (!f && bytes.empty()) throw std::runtime_error("cannot read " + path);
  const uint8_t magic[4] = {0x13, 0xab, 0xa1, 0x5c};
  if (bytes.size() != 16 + W*H*BYTES || !std::equal(magic, magic+4, bytes.begin()))
    throw std::runtime_error("invalid ASTC file " + path);
  if (bytes[4]!=8 || bytes[5]!=8 || bytes[6]!=1 || bytes[7]!=0x80 || bytes[8]!=0x07 || bytes[9]!=0 ||
      bytes[10]!=0x38 || bytes[11]!=0x04 || bytes[12]!=0)
    throw std::runtime_error("unexpected ASTC footprint or dimensions " + path);
  Image im{W,H,0,std::vector<Block>(W*H)};
  std::memcpy(im.b.data(), bytes.data()+16, im.b.size()*BYTES);
  return im;
}
Image tile_eye_pair(const Image& src) {
  // ASTC 8x8 blocks: 544x272 grid is 4352x2176 pixels, two 2176x2176 eyes.
  Image out{544,272,272,std::vector<Block>(544*272)};
  for (int y=0;y<out.h;++y) for (int x=0;x<out.w;++x) {
    int within_eye=x%out.eye_w;
    out.b[y*out.w+x] = src.b[(y%src.h)*src.w+(within_eye%src.w)];
  }
  return out;
}
Image tile_single_eye(const Image& src) {
  Image out{272,272,0,std::vector<Block>(272*272)};
  for(int y=0;y<out.h;++y) for(int x=0;x<out.w;++x)
    out.b[y*out.w+x]=src.b[(y%src.h)*src.w+(x%src.w)];
  return out;
}
uint64_t word(const Block& b, int half) {
  uint64_t v; std::memcpy(&v, b.data()+half*8, 8); return v;
}
unsigned distance(const Block& a, const Block& b) {
  return __builtin_popcountll(word(a,0)^word(b,0)) + __builtin_popcountll(word(a,1)^word(b,1));
}
void encode(const Image& ref, const Image& cur, Encoded& out) {
  if (ref.w!=cur.w || ref.h!=cur.h) throw std::runtime_error("dimension mismatch");
  const int n=ref.w*ref.h;
  out.selectors.resize(n); out.residual.resize(n*BYTES);
  for (int y=0;y<ref.h;++y) for (int x=0;x<ref.w;++x) {
    const Block& c=cur.b[y*ref.w+x]; unsigned best=129; uint8_t selected=0;
    for (int s=0;s<9;++s) {
      const int dy=s/3-1, dx=s%3-1;
      const int ny=std::clamp(y+dy,0,ref.h-1);
      const int lo=ref.eye_w ? (x/ref.eye_w)*ref.eye_w : 0;
      const int hi=ref.eye_w ? std::min(lo+ref.eye_w-1,ref.w-1) : ref.w-1;
      const int nx=std::clamp(x+dx,lo,hi);
      unsigned d=distance(c,ref.b[ny*ref.w+nx]);
      if (d<best) { best=d; selected=static_cast<uint8_t>(s); }
    }
    out.selectors[y*ref.w+x]=selected;
    const int dy=selected/3-1, dx=selected%3-1;
    const int lo=ref.eye_w ? (x/ref.eye_w)*ref.eye_w : 0;
    const int hi=ref.eye_w ? std::min(lo+ref.eye_w-1,ref.w-1) : ref.w-1;
    const Block& p=ref.b[std::clamp(y+dy,0,ref.h-1)*ref.w+std::clamp(x+dx,lo,hi)];
    for (int k=0;k<BYTES;++k) out.residual[(y*ref.w+x)*BYTES+k]=c[k]^p[k];
  }
}
void frame(const Encoded& e, std::vector<uint8_t>& raw) {
  raw.resize(e.selectors.size()+e.residual.size());
  std::copy(e.selectors.begin(),e.selectors.end(),raw.begin());
  std::copy(e.residual.begin(),e.residual.end(),raw.begin()+e.selectors.size());
}
void compress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
  static ZSTD_CCtx* ctx=ZSTD_createCCtx();
  out.resize(ZSTD_compressBound(in.size()));
  size_t n=ZSTD_compressCCtx(ctx,out.data(),out.size(),in.data(),in.size(),3);
  if (ZSTD_isError(n)) throw std::runtime_error(ZSTD_getErrorName(n));
  out.resize(n);
}
void decompress(const std::vector<uint8_t>& in, size_t size, std::vector<uint8_t>& out) {
  static ZSTD_DCtx* ctx=ZSTD_createDCtx();
  out.resize(size); size_t n=ZSTD_decompressDCtx(ctx,out.data(),out.size(),in.data(),in.size());
  if (ZSTD_isError(n) || n!=size) throw std::runtime_error("zstd decode size/error");
}
void inverse(const Image& ref,const std::vector<uint8_t>& packed, Image& out) {
  const size_t n=ref.b.size(); if (packed.size()!=n*17) throw std::runtime_error("frame size mismatch");
  out.w=ref.w; out.h=ref.h; out.eye_w=ref.eye_w; out.b.resize(n);
  const int eye_width=ref.eye_w ? ref.eye_w : ref.w;
  for (int y=0;y<ref.h;++y) {
    const int row[3]={std::max(y-1,0)*ref.w,y*ref.w,std::min(y+1,ref.h-1)*ref.w};
    for (int lo=0;lo<ref.w;lo+=eye_width) {
      const int hi=std::min(lo+eye_width,ref.w)-1;
      for (int x=lo;x<=hi;++x) {
        const size_t i=size_t(y)*ref.w+x; const unsigned s=packed[i];
        if(s>8) throw std::runtime_error("selector out of range");
        const int xx=std::clamp(x+int(s%3)-1,lo,hi);
        const Block& p=ref.b[row[s/3]+xx];
        uint64_t a,b,c,d;
        std::memcpy(&a,p.data(),8); std::memcpy(&b,p.data()+8,8);
        std::memcpy(&c,packed.data()+n+i*16,8); std::memcpy(&d,packed.data()+n+i*16+8,8);
        a^=c; b^=d;
        std::memcpy(out.b[i].data(),&a,8); std::memcpy(out.b[i].data()+8,&b,8);
      }
    }
  }
}
std::pair<double,double> stats(std::vector<double> v) {
  std::sort(v.begin(),v.end());
  auto q=[&](double p){return v[std::min(v.size()-1,(size_t)std::ceil(p*v.size())-1)];};
  return {q(.5),q(.95)};
}
Result run(const Image& ref,const Image& cur,int reps) {
  std::vector<double> enc,dec,rawtimes,rawdectimes;
  std::vector<uint8_t> raw(reinterpret_cast<const uint8_t*>(cur.b.data()),
      reinterpret_cast<const uint8_t*>(cur.b.data())+cur.b.size()*BYTES);
  Encoded e; std::vector<uint8_t> bytes,z,unpacked,rawz,rawunpacked;
  Image restored;
  compress(raw,rawz); size_t rawsize=rawz.size(),deltasize=0;
  for(int r=0;r<reps+5;++r) {
    auto tr=Clock::now(); compress(raw,rawz); auto tre=Clock::now();
    if(r>=5) rawtimes.push_back(std::chrono::duration<double,std::milli>(tre-tr).count());
    auto t=Clock::now(); encode(ref,cur,e); frame(e,bytes); compress(bytes,z);
    auto te=Clock::now(); decompress(z,bytes.size(),unpacked); inverse(ref,unpacked,restored); auto td=Clock::now();
    if(restored.b!=cur.b) throw std::runtime_error("roundtrip mismatch");
    auto trd=Clock::now(); decompress(rawz,raw.size(),rawunpacked); auto trde=Clock::now();
    if(rawunpacked!=raw) throw std::runtime_error("raw roundtrip mismatch");
    if(r>=5) {
      enc.push_back(std::chrono::duration<double,std::milli>(te-t).count());
      dec.push_back(std::chrono::duration<double,std::milli>(td-te).count());
      rawdectimes.push_back(std::chrono::duration<double,std::milli>(trde-trd).count());
    }
    deltasize=z.size();
  }
  auto a=stats(enc),b=stats(dec),c=stats(rawtimes),d=stats(rawdectimes);
  return {a.first,a.second,b.first,b.second,c.first,c.second,d.first,d.second,rawsize,deltasize};
}
void boundary_checks() {
  Image im{3,2,0,std::vector<Block>(6)};
  for(int i=0;i<6;++i) im.b[i][0]=i;
  // Every clamped 3x3 neighbor index at image edges must remain in bounds.
  for(int y=0;y<im.h;++y) for(int x=0;x<im.w;++x) for(int s=0;s<9;++s) {
    int yy=std::clamp(y+s/3-1,0,im.h-1), xx=std::clamp(x+s%3-1,0,im.w-1);
    if(yy<0||yy>=im.h||xx<0||xx>=im.w) throw std::runtime_error("boundary neighbor wrapped");
  }
  // Distinct left/right sentinels prove edge reads clamp instead of wrapping.
  if(im.b[0*im.w+std::clamp(0-1,0,im.w-1)][0]!=0 || im.b[0*im.w+std::clamp(im.w,0,im.w-1)][0]!=2)
    throw std::runtime_error("boundary clamp failed");
  im.w=4; im.eye_w=2; im.h=1; im.b.resize(4);
  if(std::clamp(1+1,0,im.eye_w-1)!=1 || std::clamp(2-1,im.eye_w,2*im.eye_w-1)!=2)
    throw std::runtime_error("stereo seam clamp failed");
}
std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream f(path,std::ios::binary);
  if(!f) throw std::runtime_error("cannot read "+path);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),{});
}
void write_file(const std::string& path,const std::vector<uint8_t>& bytes) {
  std::ofstream f(path,std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  if(!f) throw std::runtime_error("cannot write "+path);
}
uint64_t hash64(const std::vector<uint8_t>& v) {
  uint64_t h=1469598103934665603ULL;
  for(uint8_t b:v) { h^=b; h*=1099511628211ULL; }
  return h;
}
void decode_runner(const std::string& dir,const std::string& csv,int w,int h) {
  if(w!=272 || h!=272) throw std::runtime_error("fixture grid mismatch; expected 272x272 blocks");
  const size_t n=(size_t)w*h, raw_size=n*BYTES, framed_size=n*17;
  auto refbytes=read_file(dir+"/reference.raw"), packed=read_file(dir+"/packed.zstd"), expected=read_file(dir+"/expected.raw");
  if(refbytes.size()!=raw_size || expected.size()!=raw_size) throw std::runtime_error("decode fixture raw length/grid mismatch; expected 272x272x16");
  Image ref{w,h,0,std::vector<Block>(n)}; std::memcpy(ref.b.data(),refbytes.data(),raw_size);
  std::vector<uint8_t> rawz, framebytes, restoredbytes; compress(expected,rawz);
  decompress(packed,framed_size,framebytes);
  if(std::any_of(framebytes.begin(),framebytes.begin()+n,[](uint8_t s){return s>8;}))
    throw std::runtime_error("selector outside 0..8");
  Image restored;
  inverse(ref,framebytes,restored);
  if(std::memcmp(restored.b.data(),expected.data(),raw_size)!=0) throw std::runtime_error("fixture delta roundtrip mismatch");
  decompress(rawz,raw_size,restoredbytes);
  if(restoredbytes!=expected) throw std::runtime_error("fixture raw roundtrip mismatch");
  std::vector<double> delta_ms,raw_ms; std::vector<uint8_t> work; Image out;
  for(int i=0;i<35;++i) {
    auto t=Clock::now(); decompress(packed,framed_size,work); inverse(ref,work,out); auto e=Clock::now();
    if(std::memcmp(out.b.data(),expected.data(),raw_size)!=0) throw std::runtime_error("timed delta mismatch");
    auto rt=Clock::now(); decompress(rawz,raw_size,restoredbytes); auto re=Clock::now();
    if(restoredbytes!=expected) throw std::runtime_error("timed raw mismatch");
    if(i>=5) {
      delta_ms.push_back(std::chrono::duration<double,std::milli>(e-t).count());
      raw_ms.push_back(std::chrono::duration<double,std::milli>(re-rt).count());
    }
  }
  auto d=stats(delta_ms),r=stats(raw_ms);
  std::ofstream f(csv); f<<"width_blocks,height_blocks,packed_bytes,raw_bytes,reference_fnv64,expected_fnv64,delta_decode_inverse_p50_ms,delta_decode_inverse_p95_ms,raw_zstd_decode_p50_ms,raw_zstd_decode_p95_ms,exact_roundtrip\n"
    <<w<<','<<h<<','<<packed.size()<<','<<raw_size<<','<<std::hex<<hash64(refbytes)<<','<<hash64(expected)<<std::dec<<','
    <<d.first<<','<<d.second<<','<<r.first<<','<<r.second<<",true\n";
  if(!f) throw std::runtime_error("cannot write CSV");
}
void make_fixture(const std::string& root,const std::string& dir) {
  Image ref=tile_single_eye(load_astc(root+"/dark-pan0-q6.astc"));
  Image cur=tile_single_eye(load_astc(root+"/dark-pan1-q6.astc")); Encoded e; encode(ref,cur,e);
  std::vector<uint8_t> raw(reinterpret_cast<const uint8_t*>(cur.b.data()),reinterpret_cast<const uint8_t*>(cur.b.data())+cur.b.size()*BYTES);
  std::vector<uint8_t> framebytes,z,refbytes(reinterpret_cast<const uint8_t*>(ref.b.data()),reinterpret_cast<const uint8_t*>(ref.b.data())+ref.b.size()*BYTES);
  frame(e,framebytes); compress(framebytes,z);
  write_file(dir+"/reference.raw",refbytes); write_file(dir+"/packed.zstd",z); write_file(dir+"/expected.raw",raw);
}
void print(const char* name,const Result& r) {
  std::cout << "{\"name\":\""<<name<<"\",\"raw_zstd3_bytes\":"<<r.raw
    <<",\"delta_zstd3_bytes\":"<<r.delta<<",\"saving_pct\":"
    <<100.0*(static_cast<double>(r.raw)-static_cast<double>(r.delta))/r.raw
    <<",\"raw_zstd_ms_p50\":"<<r.raw50<<",\"raw_zstd_ms_p95\":"<<r.raw95
    <<",\"raw_decompress_ms_p50\":"<<r.rawdec50<<",\"raw_decompress_ms_p95\":"<<r.rawdec95
    <<",\"predict_compress_ms_p50\":"<<r.enc50
    <<",\"predict_compress_ms_p95\":"<<r.enc95<<",\"decompress_inverse_ms_p50\":"<<r.dec50
    <<",\"decompress_inverse_ms_p95\":"<<r.dec95<<",\"exact_roundtrip\":true}";
}
} // namespace

int main(int argc,char**argv) try {
  if(argc==4 && std::string(argv[1])=="--make-fixture") { make_fixture(argv[2],argv[3]); std::cout<<"fixture 272x272 single-eye written\n"; return 0; }
  if(argc==4 && std::string(argv[1])=="--decode272") { decode_runner(argv[2],argv[3],272,272); std::cout<<"decode checks passed; wrote "<<argv[3]<<"\n"; return 0; }
  if(argc==6 && std::string(argv[1])=="--decode") { decode_runner(argv[4],argv[5],std::stoi(argv[2]),std::stoi(argv[3])); std::cout<<"decode checks passed; wrote "<<argv[5]<<"\n"; return 0; }
  if(argc!=3) throw std::runtime_error("usage: native_delta [--make-fixture root dir | --decode width height dir csv] | <temporal-dir> <results.json>");
  boundary_checks();
  const std::string root=argv[1]; std::vector<std::string> records;
  for(const std::string scene:{"dark","forest"}) {
    Image ref=load_astc(root+"/"+scene+"-pan0-q6.astc");
    for(int phase:{1,4,8}) {
      Image cur=load_astc(root+"/"+scene+"-pan"+std::to_string(phase)+"-q6.astc");
      std::string name=scene+"_phase"+std::to_string(phase);
      std::ostringstream os; auto r=run(ref,cur,30); auto old=std::cout.rdbuf(os.rdbuf()); print(name.c_str(),r); std::cout.rdbuf(old); records.push_back(os.str());
      if(phase==1 && scene=="dark") {
        std::mt19937 gen(0x12345); Image random=cur;
        for(auto& b:random.b) for(auto& v:b) v=static_cast<uint8_t>(gen());
        std::ostringstream rs; auto rr=run(ref,random,30); old=std::cout.rdbuf(rs.rdbuf()); print("dark_unrelated_random",rr); std::cout.rdbuf(old); records.push_back(rs.str());
      }
      if(phase==1 && scene=="dark") {
        Image bigref=tile_eye_pair(ref),bigcur=tile_eye_pair(cur);
        std::ostringstream bs; auto br=run(bigref,bigcur,30); old=std::cout.rdbuf(bs.rdbuf()); print("stereo_proxy_4352x2176",br); std::cout.rdbuf(old); records.push_back(bs.str());
      }
    }
  }
  std::ofstream out(argv[2]); out<<"{\n  \"block_grid\":[240,135],\n  \"block_bytes\":16,\n  \"source_footprint_px\":[8,8],\n  \"selector_order\":\"dy=-1..1, dx=-1..1; clamped edges; strict-less tie break favors lower selector\",\n  \"zstd_level\":3,\n  \"timing_samples\":30,\n  \"warmups\":5,\n  \"timings_include\":\"prediction + prefix framing + Zstd compression; Zstd decompression + inverse gather/XOR\",\n  \"stereo_proxy\":\"544x272 ASTC blocks (4352x2176 pixels), two repeated 272x272 (2176x2176) eyes; predictor clamped at eye seam\",\n  \"limitations\":\"synthetic wrapped-pan ASTC input; repeated-eye proxy; no transport/reference synchronization measured\",\n  \"records\":[\n    ";
  for(size_t i=0;i<records.size();++i) out<<records[i]<<(i+1<records.size()?",\n":"\n");
  out<<"  ]\n}\n";
  std::cout<<"wrote "<<argv[2]<<" ("<<records.size()<<" records); boundary and roundtrip checks passed\n";
  return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
