#include "nxwarp_direct_lz4.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
using namespace wivrn::nxwarp_direct;
static void ok(VkResult r, const char *what) { if (r != VK_SUCCESS) throw std::runtime_error(std::string(what)+" rc="+std::to_string(r)); }
static uint64_t hash(std::span<const uint8_t> s) { uint64_t h=1469598103934665603ull; for (auto b:s) { h^=b; h*=1099511628211ull; } return h; }
static void stats(const char *name, std::vector<double> v) { std::sort(v.begin(),v.end()); auto at=[&](double q){return v[std::min(v.size()-1,size_t(q*v.size()))];}; std::cout<<name<<" p50_ms="<<at(.50)<<" p95_ms="<<at(.95)<<" n="<<v.size()<<"\n"; }
int main(int argc,char**argv) try {
 if(argc!=2) return 2;
 std::ifstream f(argv[1],std::ios::binary); std::vector<uint8_t> fixture{std::istreambuf_iterator<char>(f),{}}; if(fixture.empty()) throw std::runtime_error("empty fixture");
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="headless-map-compress-ab"; app.apiVersion=VK_API_VERSION_1_1;
 VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app; VkInstance instance{}; ok(vkCreateInstance(&ici,nullptr,&instance),"instance");
 uint32_t np=0; ok(vkEnumeratePhysicalDevices(instance,&np,nullptr),"physical count"); if(!np) throw std::runtime_error("no Vulkan physical devices"); std::vector<VkPhysicalDevice> phys(np); ok(vkEnumeratePhysicalDevices(instance,&np,phys.data()),"physical list");
 VkPhysicalDeviceProperties prop{}; vkGetPhysicalDeviceProperties(phys[0],&prop);
 uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(phys[0],&nq,nullptr); std::vector<VkQueueFamilyProperties> qf(nq); vkGetPhysicalDeviceQueueFamilyProperties(phys[0],&nq,qf.data()); uint32_t qi=0; while(qi<nq && !qf[qi].queueCount) ++qi; if(qi==nq) throw std::runtime_error("no queue family");
 float priority=1.f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex=qi; qci.queueCount=1; qci.pQueuePriorities=&priority;
 VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci; VkDevice device{}; ok(vkCreateDevice(phys[0],&dci,nullptr,&device),"device");
 VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size=fixture.size(); bci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE; VkBuffer buffer{}; ok(vkCreateBuffer(device,&bci,nullptr,&buffer),"buffer");
 VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,buffer,&req); VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(phys[0],&mp);
 uint32_t mt=UINT32_MAX; const VkMemoryPropertyFlags need=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
 for(uint32_t i=0;i<mp.memoryTypeCount;i++) if((req.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&need)==need){mt=i;break;}
 if(mt==UINT32_MAX) throw std::runtime_error("production-compatible host-visible coherent type absent");
 VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize=req.size; mai.memoryTypeIndex=mt; VkDeviceMemory mem{}; ok(vkAllocateMemory(device,&mai,nullptr,&mem),"allocate"); ok(vkBindBufferMemory(device,buffer,mem,0),"bind"); void*mapped=nullptr; ok(vkMapMemory(device,mem,0,VK_WHOLE_SIZE,0,&mapped),"map"); auto *src=static_cast<uint8_t*>(mapped); std::copy(fixture.begin(),fixture.end(),src);
 std::cout<<"device="<<prop.deviceName<<" fixture_bytes="<<fixture.size()<<" type="<<mt<<" memory_flags=0x"<<std::hex<<mp.memoryTypes[mt].propertyFlags<<std::dec<<" HOST_CACHED="<<bool(mp.memoryTypes[mt].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)<<" req_bits=0x"<<std::hex<<req.memoryTypeBits<<std::dec<<"\n";
 std::span<const uint8_t> direct(src,fixture.size()); std::vector<uint8_t> staged, out_a, out_b; std::vector<double>a,b; uint64_t ha=0,hb=0; size_t ba=0,bb=0;
 auto run=[&](bool copy,std::vector<uint8_t>&out){ auto start=std::chrono::steady_clock::now(); std::span<const uint8_t> input=direct; if(copy){staged.assign(direct.begin(),direct.end());input=staged;} auto wire=compress_lz4(input,out); auto end=std::chrono::steady_clock::now(); return std::pair<double,std::pair<uint64_t,size_t>>{std::chrono::duration<double,std::milli>(end-start).count(),{hash(wire),wire.size()}}; };
 for(int i=0;i<20;i++){(void)run(false,out_a);(void)run(true,out_b);}
 for(int i=0;i<50;i++){auto x=run(false,out_a);auto y=run(true,out_b);a.push_back(x.first);b.push_back(y.first);if(i==0){ha=x.second.first;ba=x.second.second;hb=y.second.first;bb=y.second.second;} if(x.second!=y.second) throw std::runtime_error("wire outputs differ");}
 stats("mapped_compress",a); stats("copy_then_compress",b); std::cout<<"wire_identical="<<(ha==hb&&ba==bb)<<" wire_bytes="<<ba<<" checksum=0x"<<std::hex<<ha<<std::dec<<"\n";
 vkUnmapMemory(device,mem); vkDestroyBuffer(device,buffer,nullptr); vkFreeMemory(device,mem,nullptr); vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr); return 0;
} catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
