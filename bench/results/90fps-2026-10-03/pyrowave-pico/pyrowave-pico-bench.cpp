#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include "pyrowave_decoder.h"
#include "vk/vk_allocator.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <numeric>
#include <vector>

struct Out {
    vk::raii::Image image{nullptr};
    vk::raii::DeviceMemory memory{nullptr};
    vk::raii::ImageView view{nullptr};
};
static Out make_out(vk::raii::PhysicalDevice &pd, vk::raii::Device &d, uint32_t w, uint32_t h) {
    Out o;
    vk::ImageCreateInfo ii{.sType=vk::StructureType::eImageCreateInfo,.imageType=vk::ImageType::e2D,.format=vk::Format::eR8Unorm,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,.usage=vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eColorAttachment,.sharingMode=vk::SharingMode::eExclusive,.initialLayout=vk::ImageLayout::eUndefined};
    o.image=d.createImage(ii); VkMemoryRequirements req{}; vkGetImageMemoryRequirements(VkDevice(*d),VkImage(*o.image),&req); auto mp=pd.getMemoryProperties(); uint32_t mt=UINT32_MAX;
    for(uint32_t i=0;i<mp.memoryTypeCount;i++) if((req.memoryTypeBits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&vk::MemoryPropertyFlagBits::eDeviceLocal)){mt=i;break;}
    if(mt==UINT32_MAX) throw std::runtime_error("no device-local image memory");
    o.memory=d.allocateMemory({.sType=vk::StructureType::eMemoryAllocateInfo,.allocationSize=req.size,.memoryTypeIndex=mt}); vkBindImageMemory(VkDevice(*d),VkImage(*o.image),VkDeviceMemory(*o.memory),0);
    o.view=d.createImageView({.sType=vk::StructureType::eImageViewCreateInfo,.image=*o.image,.viewType=vk::ImageViewType::e2D,.format=vk::Format::eR8Unorm,.subresourceRange={.aspectMask=vk::ImageAspectFlagBits::eColor,.baseMipLevel=0,.levelCount=1,.baseArrayLayer=0,.layerCount=1}});
    return o;
}
int main(int argc,char **argv){
 try {
    if(argc<2) throw std::runtime_error("usage: bench file.pyrowave [iterations]");
    int iterations=argc>2?std::max(1,std::atoi(argv[2])):1;
    std::ifstream f(argv[1],std::ios::binary); char magic[8]; int32_t p[8]; uint32_t n;
    if(!f.read(magic,8)||std::string(magic,8)!="PYROWAVE"||!f.read((char*)p,sizeof(p))||!f.read((char*)&n,4)) throw std::runtime_error("bad input header");
    std::vector<uint8_t> packet(n); if(!f.read((char*)packet.data(),n)) throw std::runtime_error("truncated packet");
    uint32_t w=p[0],h=p[1]; auto chroma=PyroWave::ChromaSubsampling(p[3]);
    vk::raii::Context ctx{};
    vk::ApplicationInfo ai{.sType=vk::StructureType::eApplicationInfo,.pApplicationName="PyroWave pico bench",.apiVersion=VK_API_VERSION_1_1};
    vk::raii::Instance inst(ctx,{.sType=vk::StructureType::eInstanceCreateInfo,.pApplicationInfo=&ai});
    auto pds=inst.enumeratePhysicalDevices(); auto &pd=pds.at(0); auto props=pd.getProperties();
    uint32_t qf=0; auto qps=pd.getQueueFamilyProperties(); while(qf<qps.size()&&!(qps[qf].queueFlags&vk::QueueFlagBits::eCompute))qf++;
    if(qf==qps.size()) throw std::runtime_error("no compute queue");
    auto exts=pd.enumerateDeviceExtensionProperties(); bool has=false; for(auto&e:exts) if(std::string(e.extensionName)==VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)has=true;
    const char *ext=VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME;
    vk::PhysicalDeviceSubgroupSizeControlFeaturesEXT sf{.sType=vk::StructureType::ePhysicalDeviceSubgroupSizeControlFeaturesEXT,.subgroupSizeControl=VK_TRUE,.computeFullSubgroups=VK_TRUE};
    vk::DeviceQueueCreateInfo qi{.sType=vk::StructureType::eDeviceQueueCreateInfo,.queueFamilyIndex=qf,.queueCount=1}; float priority=1; qi.pQueuePriorities=&priority;
    vk::DeviceCreateInfo di{.sType=vk::StructureType::eDeviceCreateInfo,.pNext=&sf,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi}; if(has){di.enabledExtensionCount=1;di.ppEnabledExtensionNames=&ext;}
    vk::raii::Device dev(pd,di); auto queue=dev.getQueue(qf,0);
    VmaAllocatorCreateInfo vi{}; vi.physicalDevice=*pd;vi.device=*dev;vi.instance=*inst;vi.vulkanApiVersion=VK_API_VERSION_1_1; vk_allocator allocator(vi,false);
    PyroWave::Decoder dec(pd,dev,w,h,chroma,true); PyroWave::DecoderInput input(dec);
    auto y=make_out(pd,dev,w,h), cb=make_out(pd,dev,w/2,h/2), cr=make_out(pd,dev,w/2,h/2);
    PyroWave::Decoder::ViewBuffers views{*y.view,*cb.view,*cr.view};
    vk::raii::CommandPool pool(dev,{.sType=vk::StructureType::eCommandPoolCreateInfo,.flags=vk::CommandPoolCreateFlagBits::eResetCommandBuffer,.queueFamilyIndex=qf});
    auto cmds=dev.allocateCommandBuffers({.sType=vk::StructureType::eCommandBufferAllocateInfo,.commandPool=*pool,.level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1}); auto &cmd=cmds[0];
    vk::raii::QueryPool qp(dev,{.sType=vk::StructureType::eQueryPoolCreateInfo,.queryType=vk::QueryType::eTimestamp,.queryCount=2}); vk::FenceCreateInfo fi{.sType=vk::StructureType::eFenceCreateInfo}; vk::raii::Fence fence(dev,fi);
    std::vector<double> cpu,gpu; cpu.reserve(iterations);gpu.reserve(iterations); const double ns=props.limits.timestampPeriod;
    for(int i=0;i<iterations;i++){
      input.clear(); auto t0=std::chrono::steady_clock::now(); if(!input.push_data(packet))throw std::runtime_error("push_data rejected frame");
      cmd.reset(); cmd.begin({.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit}); cmd.resetQueryPool(*qp,0,2); cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,*qp,0);
      if(!dec.decode(cmd,input,views))throw std::runtime_error("decode returned false");
      cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,*qp,1); cmd.end(); queue.submit(vk::SubmitInfo{.commandBufferCount=1,.pCommandBuffers=&*cmd},*fence); VkFence fraw=VkFence(*fence); vkWaitForFences(VkDevice(*dev),1,&fraw,VK_TRUE,UINT64_MAX);
      auto t1=std::chrono::steady_clock::now(); std::array<uint64_t,2> ts{}; vkGetQueryPoolResults(VkDevice(*dev),VkQueryPool(*qp),0,2,sizeof(ts),ts.data(),sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT); cpu.push_back(std::chrono::duration<double,std::milli>(t1-t0).count());gpu.push_back(double(ts[1]-ts[0])*ns/1e6); vkResetFences(VkDevice(*dev),1,&fraw);
    }
    auto pct=[](std::vector<double> v,double p){std::sort(v.begin(),v.end());return v[size_t((v.size()-1)*p)];};
    std::cout<<"GPU="<<props.deviceName.data()<<" api="<<VK_VERSION_MAJOR(props.apiVersion)<<"."<<VK_VERSION_MINOR(props.apiVersion)<<" frame="<<w<<"x"<<h<<" packet="<<packet.size()<<"B iterations="<<iterations<<" CPU_p50="<<pct(cpu,.50)<<"ms CPU_p95="<<pct(cpu,.95)<<"ms GPU_p50="<<pct(gpu,.50)<<"ms GPU_p95="<<pct(gpu,.95)<<"ms\n";
 } catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
