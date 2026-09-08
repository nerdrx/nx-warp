// Standalone host benchmark for the approximate PLANAR fit shader.
// The source is uploaded once; only the compute dispatches are timed.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

static void vkcheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + ": " + std::to_string(r));
}
struct B { VkBuffer b{}; VkDeviceMemory m{}; void* p{}; VkDeviceSize n{}; };
struct PC { uint32_t width, height, qp, chromaQPOffset; };
static std::vector<uint32_t> spv(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open shader");
    auto n = static_cast<size_t>(f.tellg()); std::vector<uint32_t> v(n / 4);
    f.seekg(0); f.read(reinterpret_cast<char*>(v.data()), n);
    return v;
}
struct Bench {
    VkInstance i{}; VkPhysicalDevice pd{}; VkDevice d{}; VkQueue q{};
    uint32_t fam{}, tsbits{}; float tsp{}; VkCommandPool pool{}; VkCommandBuffer cb{}; VkFence fence{};
    VkQueryPool qp{}; VkDescriptorSetLayout dsl{}; VkDescriptorPool dp{}; VkDescriptorSet ds{};
    VkPipelineLayout pl{}; VkPipeline pipe{}; B src{}, staging{}, out{};
    uint32_t tiles{};
    uint32_t mt(uint32_t bits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties p{}; vkGetPhysicalDeviceMemoryProperties(pd, &p);
        for (uint32_t n=0;n<p.memoryTypeCount;++n)
            if ((bits&(1u<<n)) && (p.memoryTypes[n].propertyFlags&want)==want) return n;
        throw std::runtime_error("no memory type");
    }
    B buf(VkDeviceSize n, VkBufferUsageFlags use, VkMemoryPropertyFlags props) {
        B x{}; x.n=n; VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=n; bi.usage=use;
        vkcheck(vkCreateBuffer(d,&bi,nullptr,&x.b),"buffer"); VkMemoryRequirements r{}; vkGetBufferMemoryRequirements(d,x.b,&r);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=r.size; ai.memoryTypeIndex=mt(r.memoryTypeBits,props);
        vkcheck(vkAllocateMemory(d,&ai,nullptr,&x.m),"memory"); vkcheck(vkBindBufferMemory(d,x.b,x.m,0),"bind");
        if (props&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) vkcheck(vkMapMemory(d,x.m,0,n,0,&x.p),"map"); return x;
    }
    void init(uint32_t w,uint32_t h,const char* shader) {
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.pApplicationName="nx-planar-fit-bench"; ai.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ii.pApplicationInfo=&ai; vkcheck(vkCreateInstance(&ii,nullptr,&i),"instance");
        uint32_t np=0; vkcheck(vkEnumeratePhysicalDevices(i,&np,nullptr),"devices"); std::vector<VkPhysicalDevice> ps(np); vkcheck(vkEnumeratePhysicalDevices(i,&np,ps.data()),"devices");
        for(auto x:ps){uint32_t nq=0;vkGetPhysicalDeviceQueueFamilyProperties(x,&nq,nullptr);std::vector<VkQueueFamilyProperties> z(nq);vkGetPhysicalDeviceQueueFamilyProperties(x,&nq,z.data());for(uint32_t k=0;k<nq;k++)if(z[k].queueFlags&VK_QUEUE_COMPUTE_BIT){pd=x;fam=k;tsbits=z[k].timestampValidBits;break;}if(pd)break;}
        if(!pd) throw std::runtime_error("no compute device"); VkPhysicalDeviceProperties prop{};vkGetPhysicalDeviceProperties(pd,&prop);tsp=prop.limits.timestampPeriod; std::fprintf(stderr,"device=%s tiles=%u\n",prop.deviceName,((w+63)/64)*((h+63)/64));
        float pr=1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=fam;qi.queueCount=1;qi.pQueuePriorities=&pr; VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;vkcheck(vkCreateDevice(pd,&di,nullptr,&d),"device");vkGetDeviceQueue(d,fam,0,&q);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pi.queueFamilyIndex=fam;pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;vkcheck(vkCreateCommandPool(d,&pi,nullptr,&pool),"pool");VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;vkcheck(vkAllocateCommandBuffers(d,&ca,&cb),"cmd");VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;vkcheck(vkCreateFence(d,&fi,nullptr,&fence),"fence");
        if(tsbits){VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=2;vkcheck(vkCreateQueryPool(d,&q,nullptr,&qp),"query");}
        tiles=((w+63)/64)*((h+63)/64); VkDeviceSize sb=VkDeviceSize(tiles)*3072*4, ob=VkDeviceSize(tiles)*26*4; staging=buf(sb,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);src=buf(sb,VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);out=buf(ob,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDescriptorSetLayoutBinding bs[2]{};for(int k=0;k<2;k++){bs[k].binding=k;bs[k].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bs[k].descriptorCount=1;bs[k].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=2;dl.pBindings=bs;vkcheck(vkCreateDescriptorSetLayout(d,&dl,nullptr,&dsl),"dsl");VkDescriptorPoolSize sz{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpci.maxSets=1;dpci.poolSizeCount=1;dpci.pPoolSizes=&sz;vkcheck(vkCreateDescriptorPool(d,&dpci,nullptr,&dp),"dp");VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=dp;da.descriptorSetCount=1;da.pSetLayouts=&dsl;vkcheck(vkAllocateDescriptorSets(d,&da,&ds),"ds");VkDescriptorBufferInfo db[2]={{src.b,0,src.n},{out.b,0,out.n}};VkWriteDescriptorSet wr[2]{};for(int k=0;k<2;k++){wr[k].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr[k].dstSet=ds;wr[k].dstBinding=k;wr[k].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;wr[k].descriptorCount=1;wr[k].pBufferInfo=&db[k];}vkUpdateDescriptorSets(d,2,wr,0,nullptr);
        auto code=spv(shader);VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=code.size()*4;sm.pCode=code.data();VkShaderModule mod{};vkcheck(vkCreateShaderModule(d,&sm,nullptr,&mod),"shader");VkPushConstantRange prc{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(PC)};VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};lc.setLayoutCount=1;lc.pSetLayouts=&dsl;lc.pushConstantRangeCount=1;lc.pPushConstantRanges=&prc;vkcheck(vkCreatePipelineLayout(d,&lc,nullptr,&pl),"layout");VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};st.stage=VK_SHADER_STAGE_COMPUTE_BIT;st.module=mod;st.pName="main";VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.stage=st;cp.layout=pl;vkcheck(vkCreateComputePipelines(d,VK_NULL_HANDLE,1,&cp,nullptr,&pipe),"pipeline");vkDestroyShaderModule(d,mod,nullptr);
    }
    void upload(){vkResetCommandBuffer(cb,0);VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};vkcheck(vkBeginCommandBuffer(cb,&bi),"begin");VkBufferCopy c{0,0,staging.n};vkCmdCopyBuffer(cb,staging.b,src.b,1,&c);VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;b.buffer=src.b;b.size=src.n;vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&b,0,nullptr);vkcheck(vkEndCommandBuffer(cb),"end");VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cb;vkcheck(vkQueueSubmit(q,1,&si,fence),"upload submit");vkcheck(vkWaitForFences(d,1,&fence,VK_TRUE,UINT64_MAX),"upload wait");vkResetFences(d,1,&fence);}
    ~Bench(){if(!d)return;vkDeviceWaitIdle(d);auto free=[&](B&x){if(x.p)vkUnmapMemory(d,x.m);if(x.b)vkDestroyBuffer(d,x.b,nullptr);if(x.m)vkFreeMemory(d,x.m,nullptr);};free(staging);free(src);free(out);if(pipe)vkDestroyPipeline(d,pipe,nullptr);if(pl)vkDestroyPipelineLayout(d,pl,nullptr);if(dp)vkDestroyDescriptorPool(d,dp,nullptr);if(dsl)vkDestroyDescriptorSetLayout(d,dsl,nullptr);if(qp)vkDestroyQueryPool(d,qp,nullptr);vkDestroyFence(d,fence,nullptr);vkDestroyCommandPool(d,pool,nullptr);vkDestroyDevice(d,nullptr);vkDestroyInstance(i,nullptr);}
};
int main(int argc,char**argv){try{if(argc<2){std::fprintf(stderr,"usage: fit-bench FIT_SPV [iterations] [flat|twotone]\n");return 2;}const uint32_t w=4352,h=2176,tx=(w+63)/64,ty=(h+63)/64,nt=tx*ty;uint32_t it=argc>2?std::stoul(argv[2]):100;if(!it) throw std::runtime_error("iterations must be positive");bool flat=argc>3&&std::string(argv[3])=="flat";Bench b;b.init(w,h,argv[1]);auto*s=static_cast<int16_t*>(b.staging.p);const size_t words=size_t(nt)*3072;for(size_t i=0;i<words*2;i++)s[i]=0;
        for(uint32_t t=0;t<nt;t++){for(uint32_t y=0;y<64;y++)for(uint32_t x=0;x<64;x++){int v=flat?128:(x<32?50:200);size_t k=size_t(t)*2048+(y*64+x)/2;s[k*2+(x&1)]=int16_t(v);}for(size_t k=0;k<512;k++){size_t u=size_t(nt)*2048+size_t(t)*512+k,v=size_t(nt)*2560+size_t(t)*512+k;int x=int(k%16)*2;int cu=flat?128:(x<16?100:150),cv=flat?128:(x<16?110:140);s[u*2]=s[u*2+1]=int16_t(cu);s[v*2]=s[v*2+1]=int16_t(cv);}}
        b.upload(); PC pc{w,h,40,0};auto a=std::chrono::steady_clock::now();vkResetCommandBuffer(b.cb,0);VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};vkcheck(vkBeginCommandBuffer(b.cb,&bi),"begin");if(b.qp)vkCmdResetQueryPool(b.cb,b.qp,0,2);if(b.qp)vkCmdWriteTimestamp(b.cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,b.qp,0);vkCmdBindPipeline(b.cb,VK_PIPELINE_BIND_POINT_COMPUTE,b.pipe);vkCmdBindDescriptorSets(b.cb,VK_PIPELINE_BIND_POINT_COMPUTE,b.pl,0,1,&b.ds,0,nullptr);vkCmdPushConstants(b.cb,b.pl,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);for(uint32_t n=0;n<it;n++){vkCmdDispatch(b.cb,nt,1,1);VkBufferMemoryBarrier bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};bar.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;bar.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;bar.buffer=b.out.b;bar.size=b.out.n;vkCmdPipelineBarrier(b.cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&bar,0,nullptr);}if(b.qp)vkCmdWriteTimestamp(b.cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,b.qp,1);vkcheck(vkEndCommandBuffer(b.cb),"end");VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&b.cb;vkcheck(vkQueueSubmit(b.q,1,&si,b.fence),"submit");vkcheck(vkWaitForFences(b.d,1,&b.fence,VK_TRUE,UINT64_MAX),"wait");double wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-a).count(),gpu=-1;if(b.qp){uint64_t q[2]{};vkcheck(vkGetQueryPoolResults(b.d,b.qp,0,2,sizeof(q),q,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT),"query");uint64_t delta=q[1]-q[0];if(b.tsbits<64)delta&=(uint64_t(1)<<b.tsbits)-1;gpu=double(delta)*b.tsp/1e6;}auto*out=static_cast<uint32_t*>(b.out.p);size_t nonzero=0;bool valid=true;for(size_t i=0;i<size_t(nt)*26;i++)nonzero+=out[i]!=0;int expected[2][3]= {{flat?128:50,flat?128:100,flat?128:110},{flat?128:200,flat?128:150,flat?128:140}};int step=161,tol=(step+31)/32;auto s8=[](uint32_t x){return x&128u?int(x)-256:int(x);};for(uint32_t t=0;t<nt;t++){size_t z=size_t(t)*26;uint32_t want=flat?0xffffffffu:0xf0f0f0f0u;if(out[z]!=0||out[z+1]!=want||out[z+2]!=want)valid=false;for(uint32_t j=3;j<17;j++)if(out[z+j])valid=false;for(uint32_t j=22;j<26;j++)if(out[z+j])valid=false;int qv[2][3]={{s8(out[z+17]&255u),s8((out[z+17]>>24)&255u),s8((out[z+18]>>16)&255u)},{s8((out[z+19]>>8)&255u),s8(out[z+20]&255u),s8((out[z+20]>>24)&255u)}};for(int r=0;r<2;r++)for(int c=0;c<3;c++){int recon=128+((qv[r][c]*step+8)>>4);if(std::abs(recon-expected[r][c])>tol)valid=false;}}std::printf("case=%s iterations=%u gpu_ms=%.3f per_dispatch_gpu_ms=%.3f wall_ms=%.3f per_dispatch_wall_ms=%.3f validation=%s output_nonzero_words=%zu/%zu\n",flat?"flat128":"twotone",it,gpu,gpu/it,wall,wall/it,valid?"pass":"FAIL",nonzero,size_t(nt)*26);return valid?0:1;}catch(const std::exception&e){std::fprintf(stderr,"fit-bench: %s\n",e.what());return 1;}}
