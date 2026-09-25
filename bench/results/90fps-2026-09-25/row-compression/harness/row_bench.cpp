#include "nxwarp_direct_zstd.h"
#include "nxwarp_direct_motion.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace wivrn::nxwarp_direct;
using bytes=std::vector<uint8_t>;
using clock_type=std::chrono::steady_clock;
static bytes readfile(const std::string&p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("cannot open fixture");return {std::istreambuf_iterator<char>(f),{}};}
static double pct(std::vector<double> v,double q){std::sort(v.begin(),v.end());return v[std::max<size_t>(1,size_t(std::ceil(v.size()*q)))-1];}
static bool decode(layout l,std::span<const uint8_t>w,bytes&out){if(is_zstd(w))return decompress_zstd(l,w,out);out.assign(w.begin(),w.end());return bool(parse_frame(l,out));}
static bytes load_raw(layout l,const std::string&path){size_t sep=path.find('|');if(sep==std::string::npos)return readfile(path);bytes old=readfile(path.substr(0,sep)),now=readfile(path.substr(sep+1));motion_native_info oi,ni;if(!build_motion_native(l,old,oi)||!build_motion_native(l,now,ni))throw std::runtime_error("invalid residual input");auto mv=estimate_motion(l,old,oi,now,ni);if(mv.hits<256)throw std::runtime_error("insufficient motion hits");bytes residual=motion_residual(l,old,oi,now,ni,mv.dx,mv.dy);if(residual.empty())throw std::runtime_error("residual generation failed");bytes check=residual;if(!restore_motion(l,old,oi,check,mv.dx,mv.dy)||check!=now)throw std::runtime_error("residual restore mismatch");return residual;}
struct fixture{std::string name,path;bool residual;bytes raw,base,candidate;};
int main(int argc,char**argv){
 if(argc<3)return 2;
 layout l{2176,2176,2,true,256,false,true,true,false,true};l.native_row_predictor=true;
 ZSTD_CCtx*bc=ZSTD_createCCtx();ZSTD_CCtx*cc=ZSTD_createCCtx();if(!bc||!cc)throw std::bad_alloc();std::vector<fixture> fs;
 for(int i=2;i<argc;++i){std::string a=argv[i];auto sep=a.find(':');if(sep==std::string::npos)return 2;fixture f;f.name=a.substr(0,sep);f.path=a.substr(sep+1);f.residual=f.path.find('|')!=std::string::npos;f.raw=load_raw(l,f.path);if(!parse_frame(l,f.raw))throw std::runtime_error("invalid NXDF fixture");
  bytes bo,bs,co,cs;auto bw=compress_zstd_predicted(f.raw,bo,bs,bc);auto cw=compress_zstd_row_predicted(l,f.raw,co,cs,cc);f.base.assign(bw.begin(),bw.end());f.candidate.assign(cw.begin(),cw.end());
  bytes dec;if(!decode(l,f.base,dec)||dec!=f.raw)throw std::runtime_error("baseline decode mismatch");if(!decode(l,f.candidate,dec)||dec!=f.raw)throw std::runtime_error("candidate decode mismatch");fs.push_back(std::move(f));
 }
 std::ofstream samples(argv[1]);samples<<"fixture,kind,phase,block,order,path,sample,decode_us,wire_bytes,exact\n";
 std::cout<<"zstd="<<ZSTD_versionString()<<",layout=2176x2176x2,order=ABBA/BAAB,blocks=12warm+12measured\n";
 std::cout<<"fixture,kind,raw_bytes,baseline_wire_bytes,candidate_wire_bytes,delta_pct,baseline_p50_us,baseline_p95_us,candidate_p50_us,candidate_p95_us,exact\n";
 uint32_t rng=0x2f916a73u;auto next=[&](){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;};
 for(auto&f:fs){std::vector<double>btime,ctime;for(int block=0;block<24;++block){bool rev=(next()&1u)!=0;std::array<int,4> order=rev?std::array<int,4>{1,0,0,1}:std::array<int,4>{0,1,1,0};for(int which:order){bytes dec;const bytes&w=which?f.candidate:f.base;auto start=clock_type::now();bool ok=decode(l,w,dec);double us=std::chrono::duration<double,std::micro>(clock_type::now()-start).count();if(!ok||dec!=f.raw)throw std::runtime_error("timed exact decode mismatch");samples<<f.name<<','<<(f.residual?"residual":"frame")<<','<<(block<12?"warm":"measured")<<','<<block<<','<<(rev?"BAAB":"ABBA")<<','<<(which?"candidate":"baseline")<<','<<(block%12*2+(which?1:0))<<','<<us<<','<<w.size()<<",1\n";if(block>=12)(which?ctime:btime).push_back(us);}}
  double delta=100.0*(double(f.candidate.size())/f.base.size()-1.0);std::cout<<f.name<<','<<(f.residual?"residual":"frame")<<','<<f.raw.size()<<','<<f.base.size()<<','<<f.candidate.size()<<','<<delta<<','<<pct(btime,.5)<<','<<pct(btime,.95)<<','<<pct(ctime,.5)<<','<<pct(ctime,.95)<<",1\n";
 }
 ZSTD_freeCCtx(bc);ZSTD_freeCCtx(cc);
}
