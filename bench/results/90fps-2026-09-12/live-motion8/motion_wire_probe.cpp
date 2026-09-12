#include "motion_field.h"
#include "wivrn_serialization.h"
#include <fstream>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <numeric>
using namespace wivrn;
int main(int argc,char**argv){
 if(argc!=3)return 2;unsigned side=std::stoul(argv[1]);if(side>512)return 2;
 motion_field_data f{.frame_idx=1,.span_ns=16666667,.width=uint16_t(side),.height=uint16_t(side)};
 std::vector<float> floats(f.value_count());std::ifstream in(argv[2],std::ios::binary);in.read((char*)floats.data(),floats.size()*4);if(!in||in.peek()!=EOF)return 3;
 for(float v:floats)if(std::isfinite(v))f.scale=std::max(f.scale,std::min(.25f,std::abs(v)));
 f.vectors.resize(f.value_count());for(size_t i=0;i<floats.size();++i)f.vectors[i]=f.scale>0&&std::isfinite(floats[i])?int8_t(std::clamp<long>(std::lround(floats[i]*(127/f.scale)),-127,127)):0;
 auto chunks=split_motion_field(f);size_t payload=0,wire=0,raw_wire=0,rle=0;
 for(auto &c:chunks){to_headset::packets message=c;serialization_packet p;p.serialize(message);size_t bytes=0;for(auto s:static_cast<std::vector<std::span<uint8_t>>&>(p))bytes+=s.size();payload+=c.vectors.size();wire+=bytes+8;raw_wire+=size_t(c.width)*c.row_count*2+(bytes-c.vectors.size())+8;rle+=c.encoding==1;}
 std::vector<double> enc,dec;motion_field_assembler a;
 for(int trial=0;trial<130;++trial){auto t0=std::chrono::steady_clock::now();auto c=split_motion_field(f);auto t1=std::chrono::steady_clock::now();motion_field_assembler b;for(auto& chunk:c)b.add(chunk);auto t2=std::chrono::steady_clock::now();if(!b.complete()||b.field().vectors!=f.vectors)return 4;if(trial>=30){enc.push_back(std::chrono::duration<double,std::micro>(t1-t0).count());dec.push_back(std::chrono::duration<double,std::micro>(t2-t1).count());}}
 std::sort(enc.begin(),enc.end());std::sort(dec.begin(),dec.end());
 std::cout<<"{\"side\":"<<side<<",\"raw_payload\":"<<f.vectors.size()<<",\"chunks\":"<<chunks.size()<<",\"rle_chunks\":"<<rle<<",\"payload\":"<<payload<<",\"wire\":"<<wire<<",\"raw_wire\":"<<raw_wire<<",\"encode_p50_us\":"<<enc[50]<<",\"encode_p95_us\":"<<enc[95]<<",\"decode_p50_us\":"<<dec[50]<<",\"decode_p95_us\":"<<dec[95]<<",\"roundtrips\":130}"<<std::endl;
}
