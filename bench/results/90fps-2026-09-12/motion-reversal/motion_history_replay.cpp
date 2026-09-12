// Offline replay of the production packed-field EMA; no future images are read.
#include "utils/motion_history.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <cmath>
using namespace wivrn;
int main(int argc,char**argv){
 if(argc<4)return 2;
 int side=std::stoi(argv[1]);std::filesystem::path out=argv[2];std::filesystem::create_directories(out);
 motion_field_data history;
 for(int j=3;j<argc;++j){
  std::vector<float> values(size_t(side)*side*4);std::ifstream in(argv[j],std::ios::binary);in.read(reinterpret_cast<char*>(values.data()),values.size()*sizeof(float));if(!in||in.peek()!=EOF)return 3;
  motion_field_data raw{.frame_idx=uint64_t(j-3),.span_ns=16'666'667,.source_time_ns=1'000'000'000+(j-3)*16'666'667LL,.source_span_ns=16'666'667,.width=uint16_t(side),.height=uint16_t(side)};
  for(auto v:values)if(std::isfinite(v))raw.scale=std::max(raw.scale,std::min(std::abs(v),.25f));
  raw.vectors.resize(values.size());
  for(size_t i=0;i<values.size();++i)raw.vectors[i]=raw.scale>0&&std::isfinite(values[i])?int8_t(std::clamp<long>(std::lround(values[i]*(127.f/raw.scale)),-127,127)):0;
  motion_field_data filtered;
  bool active=motion_history_contiguous(history,raw)&&motion_field_ema_blend(history,raw,filtered);
  history=active?std::move(filtered):raw;
  for(bool smooth:{false,true}){
   auto& f=smooth?history:raw;for(size_t i=0;i<values.size();++i)values[i]=float(f.vectors[i])/127.f*f.scale;
   std::ofstream dst(out/(std::to_string(j-3)+(smooth?"-ema.f32":"-raw.f32")),std::ios::binary);dst.write(reinterpret_cast<char*>(values.data()),values.size()*sizeof(float));if(!dst)return 4;
  }
  std::cout<<j-3<<" "<<active<<" "<<raw.scale<<" "<<history.scale<<"\n";
 }
}
