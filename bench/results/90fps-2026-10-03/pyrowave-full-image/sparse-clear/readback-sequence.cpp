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
#ifdef PYROWAVE_HAAR_FORMAT
    constexpr const char *expected_magic="PYROHAAR";
#else
    constexpr const char *expected_magic="PYROWAVE";
#endif
    if(!f.read(magic,8)||std::string(magic,8)!=expected_magic||!f.read((char*)p,sizeof(p))) throw std::runtime_error("bad input header or transform format");
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
    vk::ApplicationInfo ai{.sType=vk::StructureType::eApplicationInfo,.pApplicationName="PyroWave Pico paired-Haar compute readback",.apiVersion=VK_API_VERSION_1_1};
    vk::raii::Instance inst(ctx,{.sType=vk::StructureType::eInstanceCreateInfo,.pApplicationInfo=&ai});
    auto pds=inst.enumeratePhysicalDevices(); auto &pd=pds.at(0); auto props=pd.getProperties();
    auto [supported_features, supported11, supported12, supported_subgroup] = pd.getFeatures2<vk::PhysicalDeviceFeatures2,vk::PhysicalDeviceVulkan11Features,vk::PhysicalDeviceVulkan12Features,vk::PhysicalDeviceSubgroupSizeControlFeaturesEXT>();
    if(!supported_features.features.shaderInt16 || !supported_features.features.shaderStorageImageWriteWithoutFormat || !supported_features.features.shaderStorageImageExtendedFormats || !supported11.storageBuffer16BitAccess || !supported_subgroup.computeFullSubgroups)
      throw std::runtime_error("required storage/subgroup features unsupported: int16="+std::to_string(bool(supported_features.features.shaderInt16))+" writeWithoutFormat="+std::to_string(bool(supported_features.features.shaderStorageImageWriteWithoutFormat))+" extendedFormats="+std::to_string(bool(supported_features.features.shaderStorageImageExtendedFormats))+" storage16="+std::to_string(bool(supported11.storageBuffer16BitAccess))+" storage8="+std::to_string(bool(supported12.storageBuffer8BitAccess))+" fullSubgroups="+std::to_string(bool(supported_subgroup.computeFullSubgroups)));
    auto extensions=pd.enumerateDeviceExtensionProperties();
    if(std::ranges::none_of(extensions,[](auto const &e){return strcmp(e.extensionName.data(),VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)==0;})) throw std::runtime_error("VK_EXT_subgroup_size_control unavailable");
    auto r8_features=pd.getFormatProperties(vk::Format::eR8Unorm).optimalTilingFeatures;
    if(!(r8_features & vk::FormatFeatureFlagBits::eStorageImage) || !(r8_features & vk::FormatFeatureFlagBits::eTransferSrc)) throw std::runtime_error("R8Unorm optimal images lack storage/transfer-source support");
    uint32_t qf=0; auto qps=pd.getQueueFamilyProperties(); while(qf<qps.size()&&!(qps[qf].queueFlags&vk::QueueFlagBits::eCompute))qf++;
    if(qf==qps.size()) throw std::runtime_error("no compute queue");
    vk::DeviceQueueCreateInfo qi{.sType=vk::StructureType::eDeviceQueueCreateInfo,.queueFamilyIndex=qf,.queueCount=1}; float priority=1; qi.pQueuePriorities=&priority;
    vk::PhysicalDeviceFeatures enabled_features{};
    enabled_features.shaderInt16=VK_TRUE;
    enabled_features.shaderStorageImageWriteWithoutFormat=supported_features.features.shaderStorageImageWriteWithoutFormat;
    enabled_features.shaderStorageImageExtendedFormats=supported_features.features.shaderStorageImageExtendedFormats;
    vk::PhysicalDeviceVulkan11Features enabled11{}; enabled11.storageBuffer16BitAccess=VK_TRUE;
    vk::PhysicalDeviceVulkan12Features enabled12{}; enabled12.storageBuffer8BitAccess=supported12.storageBuffer8BitAccess; enabled12.shaderFloat16=supported12.shaderFloat16;
    vk::PhysicalDeviceSubgroupSizeControlFeaturesEXT enabled_subgroup{.sType=vk::StructureType::ePhysicalDeviceSubgroupSizeControlFeaturesEXT,.subgroupSizeControl=VK_TRUE,.computeFullSubgroups=VK_TRUE};
    enabled11.pNext=&enabled12; enabled12.pNext=&enabled_subgroup;
    const char *subgroup_ext=VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME;
    vk::DeviceCreateInfo di{.sType=vk::StructureType::eDeviceCreateInfo,.pNext=&enabled11,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.enabledExtensionCount=1,.ppEnabledExtensionNames=&subgroup_ext,.pEnabledFeatures=&enabled_features};
    vk::raii::Device dev(pd,di); auto queue=dev.getQueue(qf,0);
    VmaAllocatorCreateInfo vi{}; vi.physicalDevice=*pd;vi.device=*dev;vi.instance=*inst;vi.vulkanApiVersion=VK_API_VERSION_1_1; vk_allocator allocator(vi,false);
    PyroWave::Decoder dec(pd,dev,w,h,chroma,false); PyroWave::DecoderInput input(dec);
    const bool full_chroma=chroma==PyroWave::ChromaSubsampling::Chroma444;
    const uint32_t cw=full_chroma?w:w/2, ch=full_chroma?h:h/2;
    auto y=make_out(pd,dev,w,h), cb=make_out(pd,dev,cw,ch), cr=make_out(pd,dev,cw,ch);
    PyroWave::Decoder::ViewBuffers views{*y.view,*cb.view,*cr.view};
    vk::raii::CommandPool pool(dev,{.sType=vk::StructureType::eCommandPoolCreateInfo,.flags=vk::CommandPoolCreateFlagBits::eResetCommandBuffer,.queueFamilyIndex=qf});
    auto cmds=dev.allocateCommandBuffers({.sType=vk::StructureType::eCommandBufferAllocateInfo,.commandPool=*pool,.level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1}); auto &cmd=cmds[0];
    vk::FenceCreateInfo fi{.sType=vk::StructureType::eFenceCreateInfo}; vk::raii::Fence fence(dev,fi);
    vk::raii::QueryPool qp(dev,{.sType=vk::StructureType::eQueryPoolCreateInfo,.queryType=vk::QueryType::eTimestamp,.queryCount=2});
    std::vector<double> cpu_ms,gpu_ms; cpu_ms.reserve(30); gpu_ms.reserve(30); const double timestamp_ns=props.limits.timestampPeriod;
    const std::array<uint32_t,3> plane_w{w,cw,cw},plane_h{h,ch,ch};
    std::array<vk::DeviceSize,3> offsets{}; vk::DeviceSize total=0;
    for(size_t i=0;i<3;i++){offsets[i]=(total+3)&~vk::DeviceSize(3);total=offsets[i]+vk::DeviceSize(plane_w[i])*plane_h[i];}
    vk::raii::DeviceMemory staging_memory(nullptr); auto staging=make_readback(pd,dev,total,staging_memory);
    const std::array<Out*,3> outputs{&y,&cb,&cr};
    const std::array<const char*,3> suffix{".y.raw",".cb.raw",".cr.raw"};
    for(size_t frame=0;frame<packets.size();frame++){
      const auto cpu_start=std::chrono::steady_clock::now();
      input.clear(); if(!input.push_data(packets[frame%packets.size()]))throw std::runtime_error("push_data rejected frame");
      cmd.reset(); cmd.begin({.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
      std::array<vk::ImageMemoryBarrier,3> to_general{};
      for(size_t i=0;i<3;i++) to_general[i]={.sType=vk::StructureType::eImageMemoryBarrier,.dstAccessMask=vk::AccessFlagBits::eShaderWrite,.oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=*outputs[i]->image,.subresourceRange={.aspectMask=vk::ImageAspectFlagBits::eColor,.baseMipLevel=0,.levelCount=1,.baseArrayLayer=0,.layerCount=1}};
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eComputeShader,{}, {}, {},to_general);
      cmd.resetQueryPool(*qp,0,2); cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,*qp,0);
      if(!dec.decode(cmd,input,views))throw std::runtime_error("decode returned false");
      cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,*qp,1); cmd.end(); queue.submit(vk::SubmitInfo{.commandBufferCount=1,.pCommandBuffers=&*cmd},*fence); VkFence fraw=VkFence(*fence); vkWaitForFences(VkDevice(*dev),1,&fraw,VK_TRUE,UINT64_MAX);
      const auto cpu_stop=std::chrono::steady_clock::now(); std::array<uint64_t,2> ts{}; vkGetQueryPoolResults(VkDevice(*dev),VkQueryPool(*qp),0,2,sizeof(ts),ts.data(),sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT); cpu_ms.push_back(std::chrono::duration<double,std::milli>(cpu_stop-cpu_start).count());gpu_ms.push_back(double(ts[1]-ts[0])*timestamp_ns/1e6);
      vkResetFences(VkDevice(*dev),1,&fraw);
      cmd.reset(); cmd.begin({.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
      std::array<vk::ImageMemoryBarrier,3> to_copy{}; std::array<vk::BufferImageCopy,3> copies{};
      for(size_t i=0;i<3;i++){
        to_copy[i]={.sType=vk::StructureType::eImageMemoryBarrier,.srcAccessMask=vk::AccessFlagBits::eShaderWrite,.dstAccessMask=vk::AccessFlagBits::eTransferRead,.oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eTransferSrcOptimal,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=*outputs[i]->image,.subresourceRange={.aspectMask=vk::ImageAspectFlagBits::eColor,.baseMipLevel=0,.levelCount=1,.baseArrayLayer=0,.layerCount=1}};
        copies[i]={.bufferOffset=offsets[i],.bufferRowLength=0,.bufferImageHeight=0,.imageSubresource={.aspectMask=vk::ImageAspectFlagBits::eColor,.mipLevel=0,.baseArrayLayer=0,.layerCount=1},.imageOffset={0,0,0},.imageExtent={plane_w[i],plane_h[i],1}};
      }
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eTransfer,{}, {}, {},to_copy);
      for(size_t i=0;i<3;i++) cmd.copyImageToBuffer(*outputs[i]->image,vk::ImageLayout::eTransferSrcOptimal,*staging,copies[i]);
      vk::MemoryBarrier host_ready{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,.dstAccessMask=vk::AccessFlagBits::eHostRead};
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},host_ready,{},{}); cmd.end();
      queue.submit(vk::SubmitInfo{.commandBufferCount=1,.pCommandBuffers=&*cmd},*fence); vkWaitForFences(VkDevice(*dev),1,&fraw,VK_TRUE,UINT64_MAX);
      void *mapped_raw=nullptr; vkMapMemory(VkDevice(*dev),VkDeviceMemory(*staging_memory),0,total,0,&mapped_raw); auto mapped=static_cast<const uint8_t*>(mapped_raw);
      std::string frame_prefix=std::string(argv[2])+".frame"+(frame<10?"00":frame<100?"0":"")+std::to_string(frame);
      if(frame<packets.size()) for(size_t i=0;i<3;i++){std::ofstream out(frame_prefix+suffix[i],std::ios::binary);if(!out.write(reinterpret_cast<const char*>(mapped+offsets[i]),std::streamsize(plane_w[i])*plane_h[i]))throw std::runtime_error("failed writing readback plane");}
      vkUnmapMemory(VkDevice(*dev),VkDeviceMemory(*staging_memory));
      std::cout<<"frame="<<frame<<" prefix="<<frame_prefix<<" planes="<<uint64_t(w)*h<<","<<uint64_t(cw)*ch<<","<<uint64_t(cw)*ch<<" bytes\n";
      vkResetFences(VkDevice(*dev),1,&fraw);
    }
    auto pct=[](std::vector<double> v,double q){std::sort(v.begin(),v.end());return v[size_t((v.size()-1)*q)];};
    std::cout<<"GPU="<<props.deviceName.data()<<" api="<<VK_VERSION_MAJOR(props.apiVersion)<<"."<<VK_VERSION_MINOR(props.apiVersion)<<" frame="<<w<<"x"<<h<<" fixture_frames="<<packets.size()<<" decodes=1 warmup=0 samples=1 correctness-only path=paired-Haar compute readback=separate-not-timed cpu_definition=push_submit_fence GPU_definition=decode_timestamps CPU_p50="<<pct(cpu_ms,.50)<<"ms CPU_p95="<<pct(cpu_ms,.95)<<"ms GPU_p50="<<pct(gpu_ms,.50)<<"ms GPU_p95="<<pct(gpu_ms,.95)<<"ms\n";
 } catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
