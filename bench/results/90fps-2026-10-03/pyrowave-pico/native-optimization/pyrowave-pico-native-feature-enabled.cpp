#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include "pyrowave_decoder.h"
#include "vk/vk_allocator.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
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
    vk::ImageCreateInfo ii{.sType=vk::StructureType::eImageCreateInfo,.imageType=vk::ImageType::e2D,.format=vk::Format::eR8Unorm,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,.usage=vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eColorAttachment|vk::ImageUsageFlagBits::eTransferSrc,.sharingMode=vk::SharingMode::eExclusive,.initialLayout=vk::ImageLayout::eUndefined};
    o.image=d.createImage(ii); VkMemoryRequirements req{}; vkGetImageMemoryRequirements(VkDevice(*d),VkImage(*o.image),&req); auto mp=pd.getMemoryProperties(); uint32_t mt=UINT32_MAX;
    for(uint32_t i=0;i<mp.memoryTypeCount;i++) if((req.memoryTypeBits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&vk::MemoryPropertyFlagBits::eDeviceLocal)){mt=i;break;}
    if(mt==UINT32_MAX) throw std::runtime_error("no device-local image memory");
    o.memory=d.allocateMemory({.sType=vk::StructureType::eMemoryAllocateInfo,.allocationSize=req.size,.memoryTypeIndex=mt}); vkBindImageMemory(VkDevice(*d),VkImage(*o.image),VkDeviceMemory(*o.memory),0);
    o.view=d.createImageView({.sType=vk::StructureType::eImageViewCreateInfo,.image=*o.image,.viewType=vk::ImageViewType::e2D,.format=vk::Format::eR8Unorm,.subresourceRange={.aspectMask=vk::ImageAspectFlagBits::eColor,.baseMipLevel=0,.levelCount=1,.baseArrayLayer=0,.layerCount=1}});
    return o;
}
static vk::raii::Buffer make_readback(vk::raii::PhysicalDevice &pd, vk::raii::Device &d, vk::DeviceSize size, vk::raii::DeviceMemory &memory) {
    vk::raii::Buffer buffer(d,{.sType=vk::StructureType::eBufferCreateInfo,.size=size,.usage=vk::BufferUsageFlagBits::eTransferDst,.sharingMode=vk::SharingMode::eExclusive});
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(VkDevice(*d),VkBuffer(*buffer),&req); auto mp=pd.getMemoryProperties(); uint32_t mt=UINT32_MAX;
    for(uint32_t i=0;i<mp.memoryTypeCount;i++) if((req.memoryTypeBits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&(vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent))==(vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent)){mt=i;break;}
    if(mt==UINT32_MAX) throw std::runtime_error("no host-visible coherent readback memory");
    memory=d.allocateMemory({.sType=vk::StructureType::eMemoryAllocateInfo,.allocationSize=req.size,.memoryTypeIndex=mt});
    vkBindBufferMemory(VkDevice(*d),VkBuffer(*buffer),VkDeviceMemory(*memory),0); return buffer;
}
int main(int argc,char **argv){
 try {
    if(argc<3) throw std::runtime_error("usage: bench file.pyrowave output-prefix");
    std::ifstream f(argv[1],std::ios::binary); char magic[8]; int32_t p[8]; uint32_t n;
    if(!f.read(magic,8)||std::string(magic,8)!="PYROWAVE"||!f.read((char*)p,sizeof(p))) throw std::runtime_error("bad input header");
    std::vector<std::vector<uint8_t>> packets;
    for(;;){
      f.read((char*)&n,sizeof(n));
      if(f.gcount()==0&&f.eof()) break;
      if(f.gcount()!=sizeof(n)||!n) throw std::runtime_error("bad frame packet length");
      packets.emplace_back(n); if(!f.read((char*)packets.back().data(),n)) throw std::runtime_error("truncated frame packet");
    }
    if(packets.empty()) throw std::runtime_error("no frame packets");
    uint32_t w=p[0],h=p[1]; auto chroma=PyroWave::ChromaSubsampling(p[3]);
    vk::raii::Context ctx{};
    vk::ApplicationInfo ai{.sType=vk::StructureType::eApplicationInfo,.pApplicationName="PyroWave pico bench",.apiVersion=VK_API_VERSION_1_1};
    vk::raii::Instance inst(ctx,{.sType=vk::StructureType::eInstanceCreateInfo,.pApplicationInfo=&ai});
    auto pds=inst.enumeratePhysicalDevices(); auto &pd=pds.at(0); auto props=pd.getProperties();
    auto supported=pd.getFeatures();
    if (!supported.shaderStorageImageWriteWithoutFormat || !supported.shaderStorageImageExtendedFormats)
      throw std::runtime_error("required storage image features unsupported");
    vk::PhysicalDeviceFeatures enabled{};
    enabled.shaderStorageImageWriteWithoutFormat=supported.shaderStorageImageWriteWithoutFormat;
    enabled.shaderStorageImageExtendedFormats=supported.shaderStorageImageExtendedFormats;
    uint32_t qf=0; auto qps=pd.getQueueFamilyProperties(); while(qf<qps.size()&&!(qps[qf].queueFlags&vk::QueueFlagBits::eCompute))qf++;
    if(qf==qps.size()) throw std::runtime_error("no compute queue");
    auto exts=pd.enumerateDeviceExtensionProperties(); bool has=false; for(auto&e:exts) if(std::string(e.extensionName)==VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)has=true;
    const char *ext=VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME;
    vk::PhysicalDeviceSubgroupSizeControlFeaturesEXT sf{.sType=vk::StructureType::ePhysicalDeviceSubgroupSizeControlFeaturesEXT,.subgroupSizeControl=VK_TRUE,.computeFullSubgroups=VK_TRUE};
    vk::DeviceQueueCreateInfo qi{.sType=vk::StructureType::eDeviceQueueCreateInfo,.queueFamilyIndex=qf,.queueCount=1}; float priority=1; qi.pQueuePriorities=&priority;
    vk::DeviceCreateInfo di{.sType=vk::StructureType::eDeviceCreateInfo,.pNext=&sf,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.pEnabledFeatures=&enabled}; if(has){di.enabledExtensionCount=1;di.ppEnabledExtensionNames=&ext;}
    std::cout<<"feature shaderStorageImageWriteWithoutFormat=supported+enabled:"<<bool(enabled.shaderStorageImageWriteWithoutFormat)<<" shaderStorageImageExtendedFormats=supported+enabled:"<<bool(enabled.shaderStorageImageExtendedFormats)<<"\n";
    vk::raii::Device dev(pd,di); auto queue=dev.getQueue(qf,0);
    VmaAllocatorCreateInfo vi{}; vi.physicalDevice=*pd;vi.device=*dev;vi.instance=*inst;vi.vulkanApiVersion=VK_API_VERSION_1_1; vk_allocator allocator(vi,false);
    PyroWave::Decoder dec(pd,dev,w,h,chroma,true); PyroWave::DecoderInput input(dec);
    auto y=make_out(pd,dev,w,h), cb=make_out(pd,dev,w/2,h/2), cr=make_out(pd,dev,w/2,h/2);
    PyroWave::Decoder::ViewBuffers views{*y.view,*cb.view,*cr.view};
    vk::raii::CommandPool pool(dev,{.sType=vk::StructureType::eCommandPoolCreateInfo,.flags=vk::CommandPoolCreateFlagBits::eResetCommandBuffer,.queueFamilyIndex=qf});
    auto cmds=dev.allocateCommandBuffers({.sType=vk::StructureType::eCommandBufferAllocateInfo,.commandPool=*pool,.level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1}); auto &cmd=cmds[0];
    vk::FenceCreateInfo fi{.sType=vk::StructureType::eFenceCreateInfo}; vk::raii::Fence fence(dev,fi);
    vk::raii::QueryPool qp(dev,{.sType=vk::StructureType::eQueryPoolCreateInfo,.queryType=vk::QueryType::eTimestamp,.queryCount=2});
    std::vector<double> push_ms,record_ms,submit_ms,full_ms,gpu_ms;
    constexpr size_t warmup=12, samples=30, count=warmup+samples;
    const double timestamp_ns=props.limits.timestampPeriod;
    for(size_t frame=0;frame<count;frame++){
      auto full_start=std::chrono::steady_clock::now();
      auto t0=full_start;
      input.clear(); if(!input.push_data(packets[frame%packets.size()]))throw std::runtime_error("push_data rejected frame");
      auto t1=std::chrono::steady_clock::now();
      cmd.reset(); cmd.begin({.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit}); cmd.resetQueryPool(*qp,0,2); cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,*qp,0);
      if(!dec.decode(cmd,input,views))throw std::runtime_error("decode returned false");
      cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,*qp,1); cmd.end();
      auto t2=std::chrono::steady_clock::now();
      queue.submit(vk::SubmitInfo{.commandBufferCount=1,.pCommandBuffers=&*cmd},*fence); VkFence fraw=VkFence(*fence); vkWaitForFences(VkDevice(*dev),1,&fraw,VK_TRUE,UINT64_MAX);
      auto t3=std::chrono::steady_clock::now();
      std::array<uint64_t,2> ts{}; vkGetQueryPoolResults(VkDevice(*dev),VkQueryPool(*qp),0,2,sizeof(ts),ts.data(),sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
      if(frame>=warmup){
        push_ms.push_back(std::chrono::duration<double,std::milli>(t1-t0).count());
        record_ms.push_back(std::chrono::duration<double,std::milli>(t2-t1).count());
        submit_ms.push_back(std::chrono::duration<double,std::milli>(t3-t2).count());
        full_ms.push_back(std::chrono::duration<double,std::milli>(t3-full_start).count());
        gpu_ms.push_back(double(ts[1]-ts[0])*timestamp_ns/1e6);
      }
      vkResetFences(VkDevice(*dev),1,&fraw);
    }
    for(size_t i=0;i<push_ms.size();i++)
      std::cout<<"sample="<<i<<" push_ms="<<push_ms[i]<<" record_ms="<<record_ms[i]<<" submit_fence_ms="<<submit_ms[i]<<" cpu_total_ms="<<full_ms[i]<<" gpu_ms="<<gpu_ms[i]<<"\n";
    auto pct=[](std::vector<double> v,double q){std::sort(v.begin(),v.end());return v[size_t((v.size()-1)*q)];};
    std::cout<<"GPU="<<props.deviceName.data()<<" api="<<VK_VERSION_MAJOR(props.apiVersion)<<"."<<VK_VERSION_MINOR(props.apiVersion)<<" frame="<<w<<"x"<<h<<" fixture_frames="<<packets.size()<<" decodes="<<count<<" warmup="<<warmup<<" samples="<<samples<<" readback=none push_p50="<<pct(push_ms,.50)<<"ms push_p95="<<pct(push_ms,.95)<<"ms record_p50="<<pct(record_ms,.50)<<"ms record_p95="<<pct(record_ms,.95)<<"ms submit_fence_p50="<<pct(submit_ms,.50)<<"ms submit_fence_p95="<<pct(submit_ms,.95)<<"ms cpu_total_p50="<<pct(full_ms,.50)<<"ms cpu_total_p95="<<pct(full_ms,.95)<<"ms GPU_p50="<<pct(gpu_ms,.50)<<"ms GPU_p95="<<pct(gpu_ms,.95)<<"ms\n";
 } catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
