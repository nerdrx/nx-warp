// Standalone reconstruction overhead probe. Not a codec benchmark.
#include "vk_min.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <array>
#include <chrono>
struct Params { uint32_t width=1856,height=928,gw=464,gh=464; int32_t dx=0; uint32_t age=1,reset=0,full=0,threshold=28,origin=208,size=512; };
static_assert(sizeof(Params)==44);
void dump(const std::string& p,const void* data,size_t n){std::ofstream f(p,std::ios::binary);f.write((const char*)data,n);if(!f)throw std::runtime_error("dump failed "+p);}
int main(int argc,char**argv)try{
 if(argc<3)throw std::runtime_error("usage: probe shader.spv dump-prefix [frames=120]");
 std::string prefix=argv[2];int frames=argc>3?std::stoi(argv[3]):120;if(frames<2||frames%2)throw std::runtime_error("frames must be positive even >=2");
 std::ifstream f(argv[1],std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("SPV missing");auto n=f.tellg();if(n<=0||n%4)throw std::runtime_error("SPV length");std::vector<uint32_t> spv(size_t(n)/4);f.seekg(0);f.read((char*)spv.data(),n);
 vkmin::Device d;std::string err;auto require=[&](bool ok){if(!ok)throw std::runtime_error(err);};require(d.create(0,false,err));
 if(!d.timestamps_valid())throw std::runtime_error("GPU timestamps unavailable");
 uint32_t nq=0;vkGetPhysicalDeviceQueueFamilyProperties(d.phys(),&nq,nullptr);std::vector<VkQueueFamilyProperties> q(nq);vkGetPhysicalDeviceQueueFamilyProperties(d.phys(),&nq,q.data());auto bits=q[d.queue_family()].timestampValidBits;if(!bits)throw std::runtime_error("no timestamp valid bits");
 uint64_t mask=bits>=64?~uint64_t(0):((uint64_t(1)<<bits)-1);
 std::cout<<"device="<<d.info().name<<" timestamp_bits="<<bits<<" timestamp_period_ns="<<d.timestamp_period()<<"\n";
 constexpr size_t pixels=1856*928, guides=2*464*464;
 std::array<vkmin::Buffer,4>b;for(int i=0;i<4;i++)require(d.create_buffer((i?pixels:guides)*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true,b[i],err));
 auto G=(uint32_t*)b[0].map;auto H=(uint32_t*)b[1].map;auto F=(uint32_t*)b[2].map;auto O=(uint32_t*)b[3].map;
 // Distinct eye patterns; all input creation/upload is outside GPU timing.
 for(uint32_t y=0;y<928;y++)for(uint32_t x=0;x<1856;x++){auto i=y*1856+x;uint32_t e=x/928,l=x%928;H[i]=(l*13+y*7+e*101+(l/19)*31)%256;F[i]=(l*13+y*7+e*101+23+(y/23)*17)%256;}
 for(uint32_t e=0;e<2;e++)for(uint32_t y=0;y<464;y++)for(uint32_t x=0;x<464;x++){uint32_t i=2*y*1856+e*928+2*x;G[e*464*464+y*464+x]=(F[i]+F[i+1]+F[i+1856]+F[i+1857])/4;}
 dump(prefix+"-guide.u32",G,guides*4);dump(prefix+"-history.u32",H,pixels*4);dump(prefix+"-fresh.u32",F,pixels*4);
 vkmin::Pipeline pipe;require(d.create_pipeline(spv.data(),spv.size()*4,std::vector<VkDescriptorType>(4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),sizeof(Params),pipe,err));
 auto pool=d.create_descriptor_pool(1,4,0);auto set=d.allocate_set(pool,pipe.dsl);if(!pool||!set)throw std::runtime_error("descriptors failed");
 std::array<VkDescriptorBufferInfo,4>bi;std::array<VkWriteDescriptorSet,4>wr{};for(uint32_t i=0;i<4;i++){bi[i]={b[i].buf,0,b[i].size};wr[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr[i].dstSet=set;wr[i].dstBinding=i;wr[i].descriptorCount=1;wr[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;wr[i].pBufferInfo=&bi[i];}vkUpdateDescriptorSets(d.handle(),4,wr.data(),0,nullptr);
 auto query=d.create_timestamp_pool(2);if(!query)throw std::runtime_error("query failed");
 auto run=[&](Params p){
  auto cb=d.begin();if(!cb)throw std::runtime_error("command buffer failed");vkCmdResetQueryPool(cb,query,0,2);
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&mb,0,nullptr,0,nullptr);
  vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe.pipe);vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe.layout,0,1,&set,0,nullptr);vkCmdPushConstants(cb,pipe.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(p),&p);
  vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,query,0);vkCmdDispatch(cb,(p.width+7)/8,(p.height+7)/8,1);vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,query,1);
  d.barrier_compute_to_host(cb);require(d.submit_and_wait(cb,err));std::vector<uint64_t>ts;if(!d.read_timestamps(query,2,ts))throw std::runtime_error("query read failed");return double((ts[1]-ts[0])&mask)*d.timestamp_period()/1e6;
 };
 std::ofstream cases(prefix+"-cases.csv");cases<<"case,dx,age,reset,full,threshold,gpu_ms\n";
 std::vector<Params> tests;for(int dx:{-900,-64,-3,-1,0,1,3,64,900}){Params p;p.dx=dx;tests.push_back(p);}for(int mode=0;mode<5;mode++){Params p;p.dx=3;if(mode==0)p.reset=1;if(mode==1)p.age=0;if(mode==2)p.age=2;if(mode==3)p.full=1;if(mode==4)p.threshold=255;tests.push_back(p);}
 for(size_t i=0;i<tests.size();i++){auto p=tests[i];double ms=run(p);dump(prefix+"-case"+std::to_string(i)+".u32",O,pixels*4);cases<<i<<','<<p.dx<<','<<p.age<<','<<p.reset<<','<<p.full<<','<<p.threshold<<','<<ms<<'\n';}
 cases.close();
 // Alternating synchronized full/cheap dispatches, static resident inputs.
 // This is throughput/overhead characterization, NOT temporal image quality.
 std::ofstream timing(prefix+"-timing.csv");timing<<"frame,full,dx,gpu_ms\n";
 for(int frame=-16;frame<frames;frame++){Params p;p.full=(frame%2==0);p.dx=(frame%4<2?3:-3);auto ms=run(p);if(frame>=0)timing<<frame<<','<<p.full<<','<<p.dx<<','<<ms<<'\n';}
 timing.close();d.destroy();std::cout<<"completed: 14 dumped validation cases; "<<frames<<" timed frames; 16 warmups excluded\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
