#include "nxastc_packet_decode.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <stdexcept>
using namespace wivrn::nxastc_packet;
using Clock=std::chrono::steady_clock;
std::vector<uint8_t> read(const std::string& file){std::ifstream f(file,std::ios::binary); if(!f)throw std::runtime_error("missing input");return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char**argv)try {
 if(argc!=3)throw std::runtime_error("fixture directory and CSV required");
 std::string dir=argv[1];auto ref=read(dir+"/reference.raw"),expected=read(dir+"/expected.raw"),delta=read(dir+"/packed.zstd");
 if(ref.size()!=block_bytes(2176,2176)||expected.size()!=ref.size())throw std::runtime_error("native eye dimensions mismatch");
 std::vector<uint8_t> anchor(ZSTD_compressBound(expected.size()));auto len=ZSTD_compress(anchor.data(),anchor.size(),expected.data(),expected.size(),3);if(ZSTD_isError(len))throw std::runtime_error("compression failed");anchor.resize(len);
 auto wire=make_motion_header(2176,2176,delta.size(),compression::motion_zstd,0);
 std::vector<uint8_t> packet(wire.begin(),wire.end());packet.insert(packet.end(),delta.begin(),delta.end());auto hdr=parse_packet(packet);if(!hdr)throw std::runtime_error("invalid delta packet");
 packet_header raw{2176,2176,uint32_t(expected.size()),uint32_t(anchor.size()),compression::zstd};
 std::vector<uint8_t> out(expected.size()),scratch(expected.size()/16*17);std::vector<double> mt,rt;
 for(int i=0;i<35;i++){
  auto start=Clock::now();auto status=decode_motion_payload(*hdr,delta,ref,out,scratch);auto end=Clock::now();
  if(status!=decode_status::ok||out!=expected)throw std::runtime_error("delta not exact");
  auto rs=Clock::now();status=decode_payload(raw,anchor,out);auto re=Clock::now();
  if(status!=decode_status::ok||out!=expected)throw std::runtime_error("anchor not exact");
  if(i>=5){mt.push_back(std::chrono::duration<double,std::milli>(end-start).count());rt.push_back(std::chrono::duration<double,std::milli>(re-rs).count());}
 }
 std::sort(mt.begin(),mt.end());std::sort(rt.begin(),rt.end());
 std::ofstream f(argv[2]);f<<"width,height,raw_bytes,anchor_payload_bytes,delta_payload_bytes,delta_p50_ms,delta_p95_ms,anchor_p50_ms,anchor_p95_ms,exact\n2176,2176,"<<expected.size()<<','<<anchor.size()<<','<<delta.size()<<','<<mt[14]<<','<<mt[28]<<','<<rt[14]<<','<<rt[28]<<",true\n";
 if(!f)throw std::runtime_error("CSV write failed");std::cout<<"Production packet decode exact; CSV saved\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
