#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>
#include <vulkan/vulkan.h>
using C = std::chrono::steady_clock;
static void ck(VkResult r, const char *n) {
  if (r)
    throw std::runtime_error(std::string(n) + " " + std::to_string(r));
}
static uint64_t ns(clockid_t id) {
  timespec t{};
  clock_gettime(id, &t);
  return uint64_t(t.tv_sec) * 1000000000ull + t.tv_nsec;
}
static std::vector<uint32_t> spv(const char *p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  auto n = f.tellg();
  if (!f || n <= 0 || n % 4)
    throw std::runtime_error("bad SPV");
  std::vector<uint32_t> x(size_t(n) / 4);
  f.seekg(0);
  f.read((char *)x.data(), n);
  return x;
}
static double pct(std::vector<double> v, double p) {
  std::sort(v.begin(), v.end());
  return v[size_t((v.size() - 1) * p)];
}
struct Cal {
  uint64_t d, h, u;
};
struct Row {
  const char *k;
  double submit, waitraw, waitmono, gpu, fixed, affine, slope;
  uint64_t unc;
  bool bracket;
};
struct App {
  VkInstance in{};
  VkPhysicalDevice ph{};
  VkDevice d{};
  VkQueue q{};
  uint32_t fam{}, bits{};
  float period{};
  VkCommandPool cp{};
  VkQueryPool qp{};
  VkFence fence{};
  VkDescriptorSetLayout dsl{};
  VkPipelineLayout pl{};
  VkPipeline pipe{};
  VkShaderModule sm{};
  VkDescriptorPool dp{};
  VkDescriptorSet ds{};
  VkBuffer buf{};
  VkDeviceMemory mem{};
  PFN_vkGetCalibratedTimestampsEXT cal{};
  ~App() {
    if (d)
      vkDeviceWaitIdle(d);
    if (d) {
      if (fence)
        vkDestroyFence(d, fence, 0);
      if (qp)
        vkDestroyQueryPool(d, qp, 0);
      if (cp)
        vkDestroyCommandPool(d, cp, 0);
      if (dp)
        vkDestroyDescriptorPool(d, dp, 0);
      if (pipe)
        vkDestroyPipeline(d, pipe, 0);
      if (sm)
        vkDestroyShaderModule(d, sm, 0);
      if (pl)
        vkDestroyPipelineLayout(d, pl, 0);
      if (dsl)
        vkDestroyDescriptorSetLayout(d, dsl, 0);
      if (buf)
        vkDestroyBuffer(d, buf, 0);
      if (mem)
        vkFreeMemory(d, mem, 0);
      vkDestroyDevice(d, 0);
    }
    if (in)
      vkDestroyInstance(in, 0);
  }
  Cal getcal() {
    VkCalibratedTimestampInfoEXT i[2]{};
    i[0].sType = i[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
    i[0].timeDomain = VK_TIME_DOMAIN_DEVICE_EXT;
    i[1].timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
    uint64_t t[2]{}, u{};
    ck(cal(d, 2, i, t, &u), "cal");
    return {t[0], t[1], u};
  }
  uint64_t delta(uint64_t x, uint64_t y) const {
    uint64_t mask = bits == 64 ? UINT64_MAX : ((uint64_t(1) << bits) - 1);
    return (x - y) & mask;
  }
  int64_t sd(uint64_t x, uint64_t y) const {
    uint64_t z = delta(x, y), half = bits == 64 ? (uint64_t(1) << 63)
                                                : (uint64_t(1) << (bits - 1));
    return z >= half
               ? (bits == 64 ? int64_t(z) : int64_t(z - (uint64_t(1) << bits)))
               : int64_t(z);
  }
  uint32_t mt(uint32_t mask) {
    VkPhysicalDeviceMemoryProperties m{};
    vkGetPhysicalDeviceMemoryProperties(ph, &m);
    for (uint32_t j = 0; j < m.memoryTypeCount; j++)
      if ((mask & (1u << j)) && (m.memoryTypes[j].propertyFlags &
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        return j;
    throw std::runtime_error("no device memory");
  }
  void init(const char *shader) {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "gpu-wait-probe";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &ai;
    ck(vkCreateInstance(&ii, 0, &in), "instance");
    uint32_t n = 0;
    ck(vkEnumeratePhysicalDevices(in, &n, 0), "phys count");
    std::vector<VkPhysicalDevice> ps(n);
    ck(vkEnumeratePhysicalDevices(in, &n, ps.data()), "phys");
    for (auto p : ps) {
      VkPhysicalDeviceProperties pr{};
      vkGetPhysicalDeviceProperties(p, &pr);
      uint32_t qn = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, 0);
      std::vector<VkQueueFamilyProperties> qs(qn);
      vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, qs.data());
      for (uint32_t j = 0; j < qn; j++)
        if ((qs[j].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            qs[j].timestampValidBits &&
            std::string(pr.deviceName).find("7900 XTX") != std::string::npos) {
          ph = p;
          fam = j;
          bits = qs[j].timestampValidBits;
          period = pr.limits.timestampPeriod;
          break;
        }
      if (ph)
        break;
    }
    if (!ph)
      throw std::runtime_error("RX 7900 XTX missing");
    float pri = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = fam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &pri;
    const char *ex[] = {VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = ex;
    ck(vkCreateDevice(ph, &di, 0, &d), "device");
    vkGetDeviceQueue(d, fam, 0, &q);
    cal = (PFN_vkGetCalibratedTimestampsEXT)vkGetDeviceProcAddr(
        d, "vkGetCalibratedTimestampsEXT");
    if (!cal)
      throw std::runtime_error("cal function");
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.queueFamilyIndex = fam;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ck(vkCreateCommandPool(d, &ci, 0, &cp), "pool");
    VkQueryPoolCreateInfo qc{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qc.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qc.queryCount = 2;
    ck(vkCreateQueryPool(d, &qc, 0, &qp), "queries");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    ck(vkCreateFence(d, &fi, 0, &fence), "fence");
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo sl{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 1;
    sl.pBindings = &b;
    ck(vkCreateDescriptorSetLayout(d, &sl, 0, &dsl), "dsl");
    VkPipelineLayoutCreateInfo plci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    ck(vkCreatePipelineLayout(d, &plci, 0, &pl), "pl");
    auto code = spv(shader);
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = code.size() * 4;
    smci.pCode = code.data();
    ck(vkCreateShaderModule(d, &smci, 0, &sm), "shader");
    VkPipelineShaderStageCreateInfo ss{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ss.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ss.module = sm;
    ss.pName = "main";
    VkComputePipelineCreateInfo pi{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage = ss;
    pi.layout = pl;
    ck(vkCreateComputePipelines(d, 0, 1, &pi, 0, &pipe), "pipe");
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = 256;
    bc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    ck(vkCreateBuffer(d, &bc, 0, &buf), "buffer");
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(d, buf, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = mt(mr.memoryTypeBits);
    ck(vkAllocateMemory(d, &ma, 0, &mem), "memory");
    ck(vkBindBufferMemory(d, buf, mem, 0), "bind");
    VkDescriptorPoolSize psiz{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &psiz;
    ck(vkCreateDescriptorPool(d, &dpci, 0, &dp), "dp");
    VkDescriptorSetAllocateInfo da{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = dp;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &dsl;
    ck(vkAllocateDescriptorSets(d, &da, &ds), "ds");
    VkDescriptorBufferInfo db{buf, 0, 256};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = ds;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr.pBufferInfo = &db;
    vkUpdateDescriptorSets(d, 1, &wr, 0, 0);
  }
  VkCommandBuffer cmd(bool work) {
    VkCommandBuffer c{};
    VkCommandBufferAllocateInfo ai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cp;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    ck(vkAllocateCommandBuffers(d, &ai, &c), "alloc cmd");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    ck(vkBeginCommandBuffer(c, &bi), "begin");
    vkCmdResetQueryPool(c, qp, 0, 2);
    vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
    if (work) {
      vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
      vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds,
                              0, 0);
      vkCmdDispatch(c, 1, 1, 1);
    }
    vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
    ck(vkEndCommandBuffer(c), "end");
    return c;
  }
  Row run(VkCommandBuffer c, const char *k) {
    Cal b = getcal();
    auto sub0 = C::now();
    ck(vkResetFences(d, 1, &fence), "reset");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    ck(vkQueueSubmit(q, 1, &si, fence), "submit");
    auto sub1 = C::now();
    uint64_t w0 = ns(CLOCK_MONOTONIC), r0 = ns(CLOCK_MONOTONIC_RAW);
    ck(vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX), "wait");
    uint64_t w1 = ns(CLOCK_MONOTONIC), r1 = ns(CLOCK_MONOTONIC_RAW);
    auto q0 = C::now();
    Cal e = getcal();
    uint64_t ts[2]{};
    ck(vkGetQueryPoolResults(d, qp, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                             VK_QUERY_RESULT_64_BIT),
       "get queries");
    auto q1 = C::now();
    int64_t span = sd(e.d, b.d), off = sd(ts[1], b.d), tail = sd(ts[1], e.d);
    double slope = span ? double(e.h - b.h) / double(span) : 0;
    double endA = double(b.h) + double(off) * slope,
           endF = double(e.h) + double(tail) * period;
    return {k,
            std::chrono::duration<double, std::milli>(sub1 - sub0).count(),
            double(r1 - r0) / 1e6,
            double(w1 - w0) / 1e6,
            double(ts[1] - ts[0]) * period / 1e6,
            (double(w1) - endF) / 1e6,
            (double(w1) - endA) / 1e6,
            slope,
            std::max(b.u, e.u),
            span > 0 && off >= 0 && off <= span};
  }
};
int main(int ac, char **av) {
  try {
    if (ac != 2)
      throw std::runtime_error("usage: probe short.spv");
    App a;
    a.init(av[1]);
    auto empty = a.cmd(false), work = a.cmd(true);
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(a.ph, &p);
    std::cout << "device=" << p.deviceName << " timestampPeriod=" << a.period
              << "ns/tick validBits=" << a.bits << std::endl;
    for (int i = 0; i < 20; i++) {
      (void)a.run(empty, "empty");
      (void)a.run(work, "compute");
    }
    std::vector<Row> v;
    for (int b = 0; b < 25; b++) {
      VkCommandBuffer c[4] = {b % 2 ? work : empty, b % 2 ? empty : work,
                              b % 2 ? empty : work, b % 2 ? work : empty};
      const char *k[4] = {
          b % 2 ? "compute" : "empty", b % 2 ? "empty" : "compute",
          b % 2 ? "empty" : "compute", b % 2 ? "compute" : "empty"};
      for (int j = 0; j < 4; j++)
        v.push_back(a.run(c[j], k[j]));
    }
    std::ofstream f("probe.csv");
    f << "idx,kind,submit_ms,wait_raw_ms,wait_mono_ms,gpu_ms,fixed_margin_ms,"
         "affine_margin_ms,slope_ns_tick,max_unc_ns,bracket"
      << std::endl;
    for (size_t i = 0; i < v.size(); i++) {
      auto &r = v[i];
      f << i << ',' << r.k << ',' << r.submit << ',' << r.waitraw << ','
        << r.waitmono << ',' << r.gpu << ',' << r.fixed << ',' << r.affine
        << ',' << r.slope << ',' << r.unc << ',' << r.bracket << std::endl;
    }
    for (const char *k : {"empty", "compute"}) {
      std::vector<double> w, g, fix, aff, sl;
      int n = 0;
      for (auto &r : v)
        if (std::string(r.k) == k) {
          w.push_back(r.waitraw);
          g.push_back(r.gpu);
          fix.push_back(r.fixed);
          aff.push_back(r.affine);
          sl.push_back(r.slope);
          n += r.bracket;
        }
      std::cout << k << " n=" << w.size() << " waitRAW=" << pct(w, .5) << '/'
                << pct(w, .95) << "ms gpu=" << pct(g, .5) << '/' << pct(g, .95)
                << "ms fixedEndToWait=" << pct(fix, .5) << '/' << pct(fix, .95)
                << "ms affine=" << pct(aff, .5) << '/' << pct(aff, .95)
                << "ms slope=" << pct(sl, .5) << " bracket=" << n << '/'
                << w.size() << std::endl;
    }
    vkFreeCommandBuffers(a.d, a.cp, 1, &empty);
    vkFreeCommandBuffers(a.d, a.cp, 1, &work);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "ERROR " << e.what() << std::endl;
    return 1;
  }
}
