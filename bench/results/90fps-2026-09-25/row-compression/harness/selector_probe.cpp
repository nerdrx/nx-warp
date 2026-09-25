#include "nxwarp_direct_motion.h"
#include "nxwarp_direct_zstd.h"
#include "nxwarp_direct_lz4.h"
#include <fstream>
#include <iostream>
#include <iterator>
using namespace wivrn::nxwarp_direct;using B=std::vector<uint8_t>;
static B rd(const std::string&p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void run(layout l,const std::string&n,const B&r){B fastout,denseout,predout,scratch;auto fast=compress_lz4(r,fastout);auto dense=compress_zstd(r,denseout);auto pred=compress_zstd_predicted(r,predout,scratch);bool usepred=pred.size()*100<=dense.size()*95;if(usepred)dense=pred;const bool usedense=dense.size()*100<=fast.size()*90;auto win=usedense?dense:fast;std::cout<<n<<','<<fast.size()<<','<<denseout.size()<<','<<predout.size()<<','<<(usedense?(usepred?"zstd-v2":"zstd-v1"):"lz4")<<','<<win.size()<<'\n';}
int main(int argc,char**argv){layout l{2176,2176,2,true,256,false,true,true,false,true};std::cout<<"fixture,lz4_bytes,dense_zstd_bytes,predicted_zstd_bytes,actual_winner,winner_bytes\n";for(int i=1;i<argc;++i){std::string a=argv[i],n=a.substr(0,a.find(':')),p=a.substr(a.find(':')+1);size_t s=p.find('|');if(s==std::string::npos)run(l,n,rd(p));else{B o=rd(p.substr(0,s)),now=rd(p.substr(s+1));motion_native_info oi,ni;if(!build_motion_native(l,o,oi)||!build_motion_native(l,now,ni))throw std::runtime_error("motion parse");auto mv=estimate_motion(l,o,oi,now,ni);B res=motion_residual(l,o,oi,now,ni,mv.dx,mv.dy),chk=res;if(res.empty()||!restore_motion(l,o,oi,chk,mv.dx,mv.dy)||chk!=now)throw std::runtime_error("residual mismatch");run(l,n,res);}}}
