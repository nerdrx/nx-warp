#include "nxvc/nxvc_vk.h"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdio>
int main(int argc,char**argv){
 assert(argc==2);std::ifstream f(argv[1],std::ios::binary);std::vector<unsigned char>b((std::istreambuf_iterator<char>(f)),{});assert(!b.empty());
 uint64_t begin=123,end=456;assert(!nxvc_vk_decoder_completed_gpu_span(nullptr,&begin,&end));assert(begin==123&&end==456);
 nxvc_vkd_create_info ci;nxvc_vk_decoder_create_info_default(&ci);ci.flags=NXVC_VKD_FLAG_INDEPENDENT_TILES|NXVC_VKD_FLAG_COMPACT_CENTRE;ci.output_format=NXVC_VKD_OUT_YCBCR420;
 nxvc_vk_decoder*d=nullptr;assert(nxvc_vk_decoder_create(&ci,&d)==NXVC_VKD_OK);assert(!nxvc_vk_decoder_completed_gpu_span(d,&begin,&end));size_t off=0,n=0;assert(nxvc_vk_decoder_parse_stream_header(d,b.data(),b.size(),&off)==NXVC_VKD_OK);
 float period=0;uint32_t bits=0;nxvc_vk_decoder_timestamp_info(d,&period,&bits);assert(bits&&period>0);
 for(int i=0;i<3;++i){assert(nxvc_vk_decode_frame_ex(d,b.data()+off,b.size()-off,0,&n)==NXVC_VKD_OK);off+=n;assert(nxvc_vk_decoder_wait(d,UINT64_MAX)==NXVC_VKD_OK);assert(nxvc_vk_decoder_completed_gpu_span(d,&begin,&end));nxvc_vkd_stats st{};nxvc_vk_decoder_stats(d,&st);uint64_t mask=bits>=64?~uint64_t(0):(uint64_t(1)<<bits)-1;assert(std::abs(double((end-begin)&mask)*period/1e6-st.gpu_ms)<1e-6);}
 assert(nxvc_vk_decode_frame_ex(d,b.data(),1,0,&n)!=NXVC_VKD_OK);begin=123;end=456;assert(!nxvc_vk_decoder_completed_gpu_span(d,&begin,&end));assert(begin==123&&end==456);
 assert(nxvc_vk_decoder_parse_stream_header(d,b.data(),b.size(),&n)==NXVC_VKD_OK);assert(!nxvc_vk_decoder_completed_gpu_span(d,&begin,&end));nxvc_vk_decoder_destroy(d);puts("PASS: absent, completed, stats equality, rejected frame, stream reset");
}
