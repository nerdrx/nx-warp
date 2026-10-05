// Standalone fence retirement API gate. Not the compositor or a speed benchmark.
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
static std::atomic<int> errors{0}, warnings{0}, expected_loader_notices{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++errors;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if(type==VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT &&
           std::strstr(data->pMessage,"forced disabled because name matches filter of env var 'VK_LOADER_LAYERS_DISABLE'"))
            ++expected_loader_notices;
        else ++warnings;
    }
    std::fprintf(stderr,"validation: %s\n",data->pMessage); return VK_FALSE;
}
static void ok(VkResult r, const char* name) {
    if (r!=VK_SUCCESS) { std::fprintf(stderr,"%s: %d\n",name,int(r)); throw std::runtime_error(name); }
}
int main() {
    VkInstance instance{}; VkDebugUtilsMessengerEXT debug{}; VkDevice device{};
    VkCommandPool pool{}; VkFence fence{}; VkSemaphore gate{};
    bool blocked=false; uint64_t release_value=0;
    int failed=0;
    try {
        const char* layer="VK_LAYER_KHRONOS_validation";
        const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkValidationFeatureEnableEXT feature=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validation.enabledValidationFeatureCount=1;validation.pEnabledValidationFeatures=&feature;
        VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dbg.pNext=&validation;dbg.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbg.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbg.pfnUserCallback=message;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="NX retirement API gate";app.apiVersion=VK_API_VERSION_1_3;
        VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pNext=&dbg;ic.pApplicationInfo=&app;
        ic.enabledLayerCount=1;ic.ppEnabledLayerNames=&layer;ic.enabledExtensionCount=1;ic.ppEnabledExtensionNames=&extension;
        ok(vkCreateInstance(&ic,nullptr,&instance),"create instance");
        auto create_debug=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
        dbg.pNext=nullptr;ok(create_debug(instance,&dbg,nullptr,&debug),"create messenger");
        uint32_t count=0;ok(vkEnumeratePhysicalDevices(instance,&count,nullptr),"count devices");
        std::vector<VkPhysicalDevice> devices(count);ok(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"devices");
        VkPhysicalDevice physical{};VkPhysicalDeviceProperties props{};
        for(auto candidate:devices) { vkGetPhysicalDeviceProperties(candidate,&props);
            if(props.vendorID==0x1002 && props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {physical=candidate;break;} }
        if(!physical) throw std::runtime_error("AMD discrete device absent");
        std::printf("device=%s; api=%u.%u.%u; driver=%u\n",props.deviceName,VK_VERSION_MAJOR(props.apiVersion),VK_VERSION_MINOR(props.apiVersion),VK_VERSION_PATCH(props.apiVersion),props.driverVersion);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());uint32_t family=count;
        for(uint32_t i=0;i<count;++i)if(families[i].queueFlags&VK_QUEUE_COMPUTE_BIT){family=i;break;}
        if(family==count)throw std::runtime_error("compute queue absent");
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};f13.synchronization2=VK_TRUE;
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};f12.timelineSemaphore=VK_TRUE;f12.pNext=&f13;
        float priority=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.pNext=&f12;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;
        ok(vkCreateDevice(physical,&dc,nullptr,&device),"create device");VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;
        ok(vkCreateCommandPool(device,&pc,nullptr,&pool),"create pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        VkCommandBuffer command{};ok(vkAllocateCommandBuffers(device,&ca,&command),"allocate command");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};ok(vkCreateFence(device,&fc,nullptr,&fence),"create fence");
        VkSemaphoreTypeCreateInfo st{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};st.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};sc.pNext=&st;ok(vkCreateSemaphore(device,&sc,nullptr,&gate),"create gate");
        std::puts("generation,blocked_poll,reset_while_pending,completed_wait,completed_poll,retired_pool_reset");
        ok(vkResetCommandPool(device,pool,0),"initial pool reset");
        for(uint64_t generation=1;generation<=4;++generation) {
            VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};cb.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            ok(vkBeginCommandBuffer(command,&cb),"begin command");
            // Empty submission: test pending/retirement state, not image work or readback.
            ok(vkEndCommandBuffer(command),"end command");ok(vkResetFences(device,1,&fence),"retired fence reset");
            VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};wait.semaphore=gate;wait.value=generation;wait.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};ci.commandBuffer=command;
            VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};submit.waitSemaphoreInfoCount=1;submit.pWaitSemaphoreInfos=&wait;submit.commandBufferInfoCount=1;submit.pCommandBufferInfos=&ci;
            ok(vkQueueSubmit2(queue,1,&submit,fence),"submit");blocked=true;release_value=generation;
            const auto poll=vkWaitForFences(device,1,&fence,VK_TRUE,0);
            // Always release before checking assertions; never strand a waiting queue on failure.
            VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};signal.semaphore=gate;signal.value=generation;
            ok(vkSignalSemaphore(device,&signal),"release owned gate");blocked=false;
            const auto complete=vkWaitForFences(device,1,&fence,VK_TRUE,1000000000ULL);
            ok(complete,"complete owned submission");const auto ready=vkWaitForFences(device,1,&fence,VK_TRUE,0);
            ok(ready,"completed poll");
            const auto reuse=vkResetCommandPool(device,pool,0);ok(reuse,"retired pool reset");
            if(poll!=VK_TIMEOUT) { ++failed;std::fprintf(stderr,"blocked poll unexpectedly %d\n",int(poll)); }
            std::printf("%llu,%d,0,%d,%d,%d\n",static_cast<unsigned long long>(generation),int(poll),int(complete),int(ready),int(reuse));
        }
    } catch(const std::exception& e) {std::fprintf(stderr,"failure: %s\n",e.what());++failed;}
    if(device) {
        if(blocked) {VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};signal.semaphore=gate;signal.value=release_value;vkSignalSemaphore(device,&signal);}
        if(vkDeviceWaitIdle(device)!=VK_SUCCESS)++failed;
        if(gate)vkDestroySemaphore(device,gate,nullptr);
        if(fence)vkDestroyFence(device,fence,nullptr);
        if(pool)vkDestroyCommandPool(device,pool,nullptr);
        vkDestroyDevice(device,nullptr);
    }
    if(debug)reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,debug,nullptr);
    if(instance)vkDestroyInstance(instance,nullptr);
    std::printf("validation_errors=%d; unexpected_warnings=%d; expected_loader_filter_notices=%d; failures=%d\n",errors.load(),warnings.load(),expected_loader_notices.load(),failed);
    return failed||errors.load()||warnings.load()?1:0;
}
