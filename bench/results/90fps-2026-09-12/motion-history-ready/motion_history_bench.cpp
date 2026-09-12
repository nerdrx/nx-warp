#include "utils/motion_history.h"
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
int main(){
 for(int side : {64,128}) {
 wivrn::motion_field_data a{.span_ns=16666667,.width=uint16_t(side),.height=uint16_t(side),.scale=.1f};
 a.vectors.resize(a.value_count()); for(size_t i=0;i<a.vectors.size();++i)a.vectors[i]=int(i%255)-127;
 auto b=a; b.scale=.01f; wivrn::motion_field_data out; std::vector<double> times; long checksum=0;
 for(int i=0;i<1200;++i){b.vectors[0]=i%127;auto t=std::chrono::steady_clock::now();bool ok=wivrn::motion_field_ema_blend(a,b,out);auto e=std::chrono::steady_clock::now();if(!ok)return 1;checksum+=out.vectors[0];if(i>=200)times.push_back(std::chrono::duration<double,std::micro>(e-t).count());}
 std::sort(times.begin(),times.end());std::cout<<side<<"x"<<side<<" p50_us="<<times[500]<<" p95_us="<<times[950]<<" checksum="<<checksum<<"\n";
 }
}
