#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <lz4.h>
#include <mutex>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <vulkan/vulkan.h>
#include <zstd.h>
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}
static void ck(VkResult r, const char *s) {
  if (r != VK_SUCCESS)
    throw std::runtime_error(std::string(s) + " Vulkan " + std::to_string(r));
}
static std::vector<uint32_t> spv(const char *p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f)
    throw std::runtime_error(std::string("open ") + p);
  auto n = f.tellg();
  if (n <= 0 || n % 4)
    throw std::runtime_error("bad SPIR-V");
  std::vector<uint32_t> x(size_t(n) / 4);
  f.seekg(0);
  f.read((char *)x.data(), n);
  return x;
}
static std::vector<uint8_t> load(const char *p, size_t n) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f || uint64_t(f.tellg()) != n)
    throw std::runtime_error(std::string("input length mismatch: ") + p);
  std::vector<uint8_t> x(n);
  f.seekg(0);
  f.read((char *)x.data(), n);
  if (!f)
    throw std::runtime_error("input read");
  return x;
}
struct Buf {
  VkBuffer b{};
  VkDeviceMemory m{};
  void *map{};
};
struct Sample {
  double wall{}, record{}, submit{}, gpu0{}, gpu1{}, gpuRead0{}, gpuRead1{},
      zstd0{}, zstd1{}, pack0{}, pack1{}, launch{}, fence0{}, fence1{},
      backend0{}, backend1{};
  std::array<double, 2> senderBefore{}, senderAfter{};
  std::array<std::shared_ptr<const std::vector<uint8_t>>, 2> packet;
  std::vector<uint8_t> raw0, raw1;
};
struct Compressor {
  ZSTD_CCtx *ctx = ZSTD_createCCtx();
  std::vector<char> zbuf, lbuf;
  Compressor() {
    if (!ctx)
      throw std::runtime_error("Zstd context");
  }
  ~Compressor() { ZSTD_freeCCtx(ctx); }
};
struct Eye {
  VkImage image{};
  VkDeviceMemory im{};
  VkImageView view{};
  Buf staging, output, readback;
  VkDescriptorSet ds{};
  VkCommandBuffer cmd{};
  VkFence fence{};
  VkQueryPool query{};
};
struct App {
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family{}, validBits{};
  float timestampPeriod{};
  VkPhysicalDeviceMemoryProperties mem{};
  VkCommandPool pool{};
  VkSampler sampler{};
  VkDescriptorSetLayout dsl{};
  VkPipelineLayout pl{};
  VkPipeline pipeline{};
  VkShaderModule shader{};
  VkDescriptorPool dp{};
  std::array<Eye, 2> eye{};
  uint32_t sw = 2176, sh = 2176, tw = 2176, th = 2176;
  uint64_t blocks{}, outBytes{};
  ~App() {
    if (device)
      vkDeviceWaitIdle(device);
    if (device) {
      for (auto &e : eye) {
        if (e.query)
          vkDestroyQueryPool(device, e.query, 0);
        if (e.fence)
          vkDestroyFence(device, e.fence, 0);
        if (e.view)
          vkDestroyImageView(device, e.view, 0);
        if (e.image)
          vkDestroyImage(device, e.image, 0);
        if (e.im)
          vkFreeMemory(device, e.im, 0);
        for (Buf *b : {&e.staging, &e.output, &e.readback}) {
          if (b->map)
            vkUnmapMemory(device, b->m);
          if (b->b)
            vkDestroyBuffer(device, b->b, 0);
          if (b->m)
            vkFreeMemory(device, b->m, 0);
        }
      }
      if (pool)
        vkDestroyCommandPool(device, pool, 0);
      if (dp)
        vkDestroyDescriptorPool(device, dp, 0);
      if (pipeline)
        vkDestroyPipeline(device, pipeline, 0);
      if (shader)
        vkDestroyShaderModule(device, shader, 0);
      if (pl)
        vkDestroyPipelineLayout(device, pl, 0);
      if (dsl)
        vkDestroyDescriptorSetLayout(device, dsl, 0);
      if (sampler)
        vkDestroySampler(device, sampler, 0);
      vkDestroyDevice(device, 0);
    }
    if (instance)
      vkDestroyInstance(instance, 0);
  }
  uint32_t mt(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
      if ((bits & (1u << i)) &&
          (mem.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("no memory type");
  }
  Buf buffer(VkDeviceSize n, VkBufferUsageFlags usage,
             VkMemoryPropertyFlags flags, bool map) {
    Buf x;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = n;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ck(vkCreateBuffer(device, &ci, 0, &x.b), "create buffer");
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, x.b, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt(mr.memoryTypeBits, flags);
    ck(vkAllocateMemory(device, &ai, 0, &x.m), "allocate buffer");
    ck(vkBindBufferMemory(device, x.b, x.m, 0), "bind buffer");
    if (map)
      ck(vkMapMemory(device, x.m, 0, VK_WHOLE_SIZE, 0, &x.map), "map buffer");
    return x;
  }
  void upload(Eye &e, const std::vector<uint8_t> &src) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ck(vkBeginCommandBuffer(e.cmd, &bi), "begin upload");
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = e.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(e.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {sw, sh, 1};
    vkCmdCopyBufferToImage(e.cmd, e.staging.b, e.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(e.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, 0, 0, 0, 1,
                         &b);
    ck(vkEndCommandBuffer(e.cmd), "end upload");
    VkFence f{};
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    ck(vkCreateFence(device, &fi, 0, &f), "upload fence");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &e.cmd;
    ck(vkQueueSubmit(queue, 1, &si, f), "upload submit");
    ck(vkWaitForFences(device, 1, &f, VK_TRUE, UINT64_MAX), "upload wait");
    vkDestroyFence(device, f, 0);
    ck(vkResetCommandBuffer(e.cmd, 0), "reset after upload");
  }
  void record(Eye &e) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ck(vkBeginCommandBuffer(e.cmd, &bi), "begin dispatch");
    vkCmdResetQueryPool(e.cmd, e.query, 0, 4);
    vkCmdWriteTimestamp(e.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, e.query, 0);
    vkCmdWriteTimestamp(e.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, e.query,
                        1);
    vkCmdBindPipeline(e.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(e.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1,
                            &e.ds, 0, 0);
    uint32_t pc[5]{tw, th, 3, 6, 1};
    vkCmdPushConstants(e.cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc),
                       pc);
    vkCmdDispatch(e.cmd, uint32_t((blocks + 63) / 64), 1, 1);
    vkCmdWriteTimestamp(e.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, e.query,
                        2);
    VkBufferMemoryBarrier ob{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    ob.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    ob.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ob.srcQueueFamilyIndex = ob.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ob.buffer = e.output.b;
    ob.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(e.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 1, &ob, 0, 0);
    VkBufferCopy bc{0, 0, outBytes};
    vkCmdCopyBuffer(e.cmd, e.output.b, e.readback.b, 1, &bc);
    VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.buffer = e.readback.b;
    hb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(e.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, 0, 1, &hb, 0, 0);
    vkCmdWriteTimestamp(e.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, e.query,
                        3);
    ck(vkEndCommandBuffer(e.cmd), "end dispatch");
  }
  double query(Eye &e, int x, int y) {
    uint64_t q[4];
    ck(vkGetQueryPoolResults(device, e.query, 0, 4, sizeof(q), q,
                             sizeof(uint64_t),
                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
       "query results");
    uint64_t mask =
        validBits == 64 ? UINT64_MAX : ((uint64_t(1) << validBits) - 1);
    return double((q[y] - q[x]) & mask) * timestampPeriod / 1e6;
  }
};
static std::vector<uint8_t>
pack_packet(Compressor &c, const std::vector<uint8_t> &raw, double &zms) {
  auto t = Clock::now();
  c.zbuf.resize(ZSTD_compressBound(raw.size()));
  size_t zn = ZSTD_compressCCtx(c.ctx, c.zbuf.data(), c.zbuf.size(), raw.data(),
                                raw.size(), 3);
  auto ze = Clock::now();
  zms = ms(t, ze);
  bool zok = !ZSTD_isError(zn) && zn > 0;
  int ln = 0;
  if (!zok || zn > raw.size() / 2) {
    c.lbuf.resize(LZ4_compressBound(int(raw.size())));
    ln = LZ4_compress_default((char *)raw.data(), c.lbuf.data(),
                              int(raw.size()), int(c.lbuf.size()));
  }
  uint8_t enc = 0;
  const uint8_t *data = raw.data();
  size_t n = raw.size();
  if (ln > 0 && size_t(ln) < n) {
    enc = 1;
    data = (const uint8_t *)c.lbuf.data();
    n = ln;
  }
  if (zok && zn * 100 <= n * 90) {
    enc = 2;
    data = (const uint8_t *)c.zbuf.data();
    n = zn;
  }
  std::vector<uint8_t> out(24 + n);
  out[0] = 'N';
  out[1] = 'A';
  out[2] = 'S';
  out[3] = 'T';
  out[4] = enc == 2 ? 2 : 1;
  out[5] = enc;
  auto w32 = [&](int o, uint32_t v) {
    for (int i = 0; i < 4; i++)
      out[o + i] = uint8_t(v >> (i * 8));
  };
  w32(8, 2176);
  w32(12, 2176);
  w32(16, uint32_t(raw.size()));
  w32(20, uint32_t(n));
  std::memcpy(out.data() + 24, data, n);
  return out;
}
static bool verify_packet(const std::vector<uint8_t> &p,
                          const std::vector<uint8_t> &raw) {
  if (p.size() < 24 || p[0] != 'N' || p[1] != 'A' || p[2] != 'S' ||
      p[3] != 'T' || p[5] > 2)
    return false;
  auto bytes = std::span<const uint8_t>(p.data() + 24, p.size() - 24);
  std::vector<uint8_t> d(raw.size());
  if (p[5] == 0) {
    if (bytes.size() != d.size())
      return false;
    std::copy(bytes.begin(), bytes.end(), d.begin());
  } else if (p[5] == 1) {
    if (LZ4_decompress_safe((const char *)bytes.data(), (char *)d.data(),
                            int(bytes.size()), int(d.size())) != int(d.size()))
      return false;
  } else if (ZSTD_decompress(d.data(), d.size(), bytes.data(), bytes.size()) !=
             d.size())
    return false;
  return d == raw;
}

struct TxPacket {
  uint64_t frame{};
  uint32_t eye{}, width = 2176, height = 2176;
  std::shared_ptr<const std::vector<uint8_t>> bytes;
};
struct PacedSender {
  struct Job {
    int mode;
    std::shared_ptr<const TxPacket> packet;
  };
  std::array<double, 5> mbps{};
  std::mutex m;
  std::condition_variable cv;
  std::deque<Job> q;
  int activeMode = -1, activeEye = -1;
  bool stop = false;
  std::thread worker;
  PacedSender() : worker([this] { loop(); }) {}
  ~PacedSender() {
    {
      std::lock_guard l(m);
      stop = true;
    }
    cv.notify_all();
    worker.join();
  }
  void loop() {
    for (;;) {
      Job j;
      double rate;
      {
        std::unique_lock l(m);
        cv.wait(l, [&] { return stop || !q.empty(); });
        if (stop && q.empty())
          return;
        j = std::move(q.front());
        q.pop_front();
        activeMode = j.mode;
        activeEye = int(j.packet->eye);
        rate = mbps[j.mode];
      }
      if (rate > 0)
        std::this_thread::sleep_for(std::chrono::duration<double>(
            double(j.packet->bytes->size()) * 8 / (rate * 1e6)));
      {
        std::lock_guard l(m);
        activeMode = activeEye = -1;
      }
      cv.notify_all();
    }
  }
  void set_rate(int mode, double rate) {
    std::lock_guard l(m);
    mbps[mode] = rate;
  }
  double wait_idle(int mode, int eye) {
    auto t = Clock::now();
    std::unique_lock l(m);
    cv.wait(l, [&] {
      return !(activeMode == mode && activeEye == eye) &&
             std::ranges::none_of(q, [&](auto &j) {
               return j.mode == mode && j.packet->eye == uint32_t(eye);
             });
    });
    return ms(t, Clock::now());
  }
  void wait_all() {
    std::unique_lock l(m);
    cv.wait(l, [&] { return q.empty() && activeMode < 0; });
  }
  void send(int mode, std::shared_ptr<const TxPacket> p) {
    {
      std::lock_guard l(m);
      q.push_back({mode, std::move(p)});
    }
    cv.notify_all();
  }
};
static Sample
run(App &a, Compressor &z0, Compressor &z1, bool parallel,
    const std::function<double(int)> &before,
    const std::function<double(
        int, const std::shared_ptr<const std::vector<uint8_t>> &)> &after) {
  auto begin = Clock::now();
  auto r0 = Clock::now();
  a.record(a.eye[0]);
  a.record(a.eye[1]);
  auto recordEnd = Clock::now();
  ck(vkResetFences(
         a.device, 2,
         std::array<VkFence, 2>{a.eye[0].fence, a.eye[1].fence}.data()),
     "reset fences");
  VkSubmitInfo si0{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si0.commandBufferCount = 1;
  si0.pCommandBuffers = &a.eye[0].cmd;
  auto s0 = Clock::now();
  ck(vkQueueSubmit(a.queue, 1, &si0, a.eye[0].fence), "submit eye0");
  VkSubmitInfo si1{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si1.commandBufferCount = 1;
  si1.pCommandBuffers = &a.eye[1].cmd;
  ck(vkQueueSubmit(a.queue, 1, &si1, a.eye[1].fence), "submit eye1");
  auto subEnd = Clock::now();
  Sample s;
  s.record = ms(r0, recordEnd);
  s.submit = ms(s0, subEnd);
  auto readpack = [&](int i, Compressor &z, double &fence, double &gpu,
                      double &gpuread, double &zms, double &pk, double &backend,
                      std::vector<uint8_t> &raw,
                      std::shared_ptr<const std::vector<uint8_t>> &packet) {
    s.senderBefore[i] = before(i);
    auto start = Clock::now();
    auto w = Clock::now();
    ck(vkWaitForFences(a.device, 1, &a.eye[i].fence, VK_TRUE, UINT64_MAX),
       "eye fence wait");
    fence = ms(w, Clock::now());
    VkMappedMemoryRange inv{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    inv.memory = a.eye[i].readback.m;
    inv.size = VK_WHOLE_SIZE;
    ck(vkInvalidateMappedMemoryRanges(a.device, 1, &inv),
       "invalidate readback");
    gpu = a.query(a.eye[i], 1, 2);
    gpuread = a.query(a.eye[i], 0, 3);
    raw.resize(a.outBytes);
    std::memcpy(raw.data(), a.eye[i].readback.map, a.outBytes);
    auto p = Clock::now();
    auto packed = pack_packet(z, raw, zms);
    pk = ms(p, Clock::now());
    packet = std::make_shared<const std::vector<uint8_t>>(std::move(packed));
    backend = ms(start, Clock::now());
    s.senderAfter[i] = after(i, packet);
  };
  if (parallel) {
    auto launch = Clock::now();
    auto fut = std::async(std::launch::async, [&] {
      readpack(1, z1, s.fence1, s.gpu1, s.gpuRead1, s.zstd1, s.pack1,
               s.backend1, s.raw1, s.packet[1]);
    });
    s.launch = ms(launch, Clock::now());
    readpack(0, z0, s.fence0, s.gpu0, s.gpuRead0, s.zstd0, s.pack0, s.backend0,
             s.raw0, s.packet[0]);
    fut.get();
  } else {
    readpack(0, z0, s.fence0, s.gpu0, s.gpuRead0, s.zstd0, s.pack0, s.backend0,
             s.raw0, s.packet[0]);
    readpack(1, z1, s.fence1, s.gpu1, s.gpuRead1, s.zstd1, s.pack1, s.backend1,
             s.raw1, s.packet[1]);
  }
  s.wall = ms(begin, Clock::now());
  return s;
}
static double pct(std::vector<double> v, double p) {
  std::sort(v.begin(), v.end());
  return v[size_t((v.size() - 1) * p)];
}

int main(int ac, char **av) {
  try {
    if (ac != 4)
      throw std::runtime_error(
          "usage: stereo-gpu dark.rgba forest.rgba output-dir");
    std::string out = av[3];
    if (out.size() && out.back() != '/')
      out += '/';
    constexpr uint32_t sw = 2176, sh = 2176;
    App a;
    a.blocks = uint64_t(sw / 8) * (sh / 8);
    a.outBytes = a.blocks * 16;
    auto src0 = load(av[1], size_t(sw) * sh * 4),
         src1 = load(av[2], size_t(sw) * sh * 4);
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "stereo-gpu-bench";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &ai;
    ck(vkCreateInstance(&ii, 0, &a.instance), "instance");
    uint32_t n = 0;
    ck(vkEnumeratePhysicalDevices(a.instance, &n, nullptr), "devices");
    std::vector<VkPhysicalDevice> ps(n);
    ck(vkEnumeratePhysicalDevices(a.instance, &n, ps.data()), "devices");
    int best = -1;
    VkPhysicalDeviceProperties props{};
    for (auto p : ps) {
      VkPhysicalDeviceProperties q;
      vkGetPhysicalDeviceProperties(p, &q);
      uint32_t nq = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(p, &nq, nullptr);
      std::vector<VkQueueFamilyProperties> qs(nq);
      vkGetPhysicalDeviceQueueFamilyProperties(p, &nq, qs.data());
      for (uint32_t j = 0; j < nq; j++)
        if ((qs[j].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            qs[j].timestampValidBits) {
          int score =
              q.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 10 : 0;
          if (std::string(q.deviceName).find("7900 XTX") != std::string::npos)
            score += 100;
          if (score > best) {
            best = score;
            a.physical = p;
            a.family = j;
            a.validBits = qs[j].timestampValidBits;
            props = q;
          }
        }
    }
    if (best < 0)
      throw std::runtime_error("no timestamped compute queue");
    a.timestampPeriod = props.limits.timestampPeriod;
    vkGetPhysicalDeviceMemoryProperties(a.physical, &a.mem);
    float pri = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = a.family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &pri;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    ck(vkCreateDevice(a.physical, &di, 0, &a.device), "device");
    vkGetDeviceQueue(a.device, a.family, 0, &a.queue);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ck(vkCreateSampler(a.device, &sci, 0, &a.sampler), "sampler");
    VkDescriptorSetLayoutBinding bind[3]{};
    for (int i = 0; i < 3; i++) {
      bind[i].binding = i;
      bind[i].descriptorCount = 1;
      bind[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      bind[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                     : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo dl{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 3;
    dl.pBindings = bind;
    ck(vkCreateDescriptorSetLayout(a.device, &dl, 0, &a.dsl),
       "descriptor layout");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &a.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    ck(vkCreatePipelineLayout(a.device, &pl, 0, &a.pl), "pipeline layout");
    auto code = spv("build/encode_primary.spv");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = code.size() * 4;
    sm.pCode = code.data();
    ck(vkCreateShaderModule(a.device, &sm, 0, &a.shader), "shader");
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = a.shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo pi{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage = stage;
    pi.layout = a.pl;
    ck(vkCreateComputePipelines(a.device, VK_NULL_HANDLE, 1, &pi, nullptr,
                                &a.pipeline),
       "pipeline");
    VkDescriptorPoolSize sizes[2]{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo dpi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    ck(vkCreateDescriptorPool(a.device, &dpi, 0, &a.dp), "descriptor pool");
    std::array<VkDescriptorSetLayout, 2> layouts{a.dsl, a.dsl};
    VkDescriptorSetAllocateInfo dai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = a.dp;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = layouts.data();
    std::array<VkDescriptorSet, 2> sets{};
    ck(vkAllocateDescriptorSets(a.device, &dai, sets.data()),
       "descriptor sets");
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = a.family;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ck(vkCreateCommandPool(a.device, &cpi, 0, &a.pool), "command pool");
    std::array<VkCommandBuffer, 2> cmds{};
    VkCommandBufferAllocateInfo cai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = a.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 2;
    ck(vkAllocateCommandBuffers(a.device, &cai, cmds.data()), "commands");
    std::array<const std::vector<uint8_t> *, 2> src{&src0, &src1};
    for (int i = 0; i < 2; i++) {
      auto &e = a.eye[i];
      e.ds = sets[i];
      e.cmd = cmds[i];
      VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ic.imageType = VK_IMAGE_TYPE_2D;
      ic.format = VK_FORMAT_R8G8B8A8_UNORM;
      ic.extent = {sw, sh, 1};
      ic.mipLevels = ic.arrayLayers = 1;
      ic.samples = VK_SAMPLE_COUNT_1_BIT;
      ic.tiling = VK_IMAGE_TILING_OPTIMAL;
      ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      ck(vkCreateImage(a.device, &ic, 0, &e.image), "image");
      VkMemoryRequirements mr;
      vkGetImageMemoryRequirements(a.device, e.image, &mr);
      VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ma.allocationSize = mr.size;
      ma.memoryTypeIndex =
          a.mt(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      ck(vkAllocateMemory(a.device, &ma, 0, &e.im), "image memory");
      ck(vkBindImageMemory(a.device, e.image, e.im, 0), "bind image");
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = e.image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = ic.format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      ck(vkCreateImageView(a.device, &vi, 0, &e.view), "view");
      e.staging = a.buffer(src[i]->size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           true);
      memcpy(e.staging.map, src[i]->data(), src[i]->size());
      e.output = a.buffer(a.outBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
      e.readback = a.buffer(a.outBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                            true);
      VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      ck(vkCreateFence(a.device, &fi, 0, &e.fence), "fence");
      VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qci.queryCount = 4;
      ck(vkCreateQueryPool(a.device, &qci, 0, &e.query), "query pool");
      VkDescriptorImageInfo im{a.sampler, e.view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorBufferInfo bo{e.output.b, 0, a.outBytes};
      VkWriteDescriptorSet wr[3]{};
      for (int j = 0; j < 3; j++) {
        wr[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[j].dstSet = e.ds;
        wr[j].dstBinding = j;
        wr[j].descriptorCount = 1;
        if (j < 2) {
          wr[j].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          wr[j].pImageInfo = &im;
        } else {
          wr[j].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          wr[j].pBufferInfo = &bo;
        }
      }
      vkUpdateDescriptorSets(a.device, 3, wr, 0, nullptr);
      a.upload(e, *src[i]);
    }
    Compressor z0, z1;
    constexpr int warm = 20, count = 100, batch = 25, modes = 5;
    std::array<double, modes> bitrate{250, 250, 500, 500, 0};
    std::array<const char *, modes> names{
        "legacy_250", "deferred_250", "legacy_500", "deferred_500", "unpaced"};
    std::ofstream csv(out + "sender-wait.csv");
    csv << "order,mode,frame,bitrate_mbps,cycle_ms,record_ms,two_submit_api_ms,"
           "sender_before_eye0_ms,sender_before_eye1_ms,sender_after_eye0_ms,"
           "sender_after_eye1_ms,backend_eye0_ms,backend_eye1_ms,fence_eye0_ms,"
           "fence_eye1_ms,eye0_zstd_ms,eye1_zstd_ms,eye0_pack_ms,eye1_pack_ms,"
           "eye0_gpu_dispatch_ms,eye1_gpu_dispatch_ms,eye0_dispatch_readback_"
           "ms,eye1_dispatch_readback_ms,packet_eye0_bytes,packet_eye1_bytes,"
           "old_packet_metadata_unchanged\n";
    PacedSender tx;
    std::array<std::array<std::shared_ptr<const TxPacket>, 2>, modes>
        previous{};
    std::array<int, modes> frameNo{};
    struct Row {
      int order, mode, frame;
      double cycle, record, submit, before0, before1, after0, after1, backend0,
          backend1, fence0, fence1, zstd0, zstd1, pack0, pack1, gpu0, gpu1,
          gpuRead0, gpuRead1;
      size_t p0, p1;
      bool intact;
      std::array<std::vector<uint8_t>, 2> raw;
      std::array<std::shared_ptr<const std::vector<uint8_t>>, 2> packet;
    };
    std::vector<Row> rows;
    std::array<std::array<std::vector<uint8_t>, 2>, modes> rawBaseline,
        packetBaseline;
    int order = 0;
    auto step = [&](int mode, bool measured, std::vector<Row> &batchRows) {
      int frame = frameNo[mode];
      auto old = previous[mode];
      std::array<bool, 2> intact{true, true};
      bool legacy = mode == 0 || mode == 2;
      bool deferred = mode == 1 || mode == 3;
      auto before = [&](int eye) {
        return legacy ? tx.wait_idle(mode, eye) : 0.0;
      };
      auto after = [&](int eye,
                       const std::shared_ptr<const std::vector<uint8_t>>
                           &packet) {
        auto prior = old[eye];
        intact[eye] =
            prior == previous[mode][eye] &&
            (!prior || (prior->frame == uint64_t(frame - 1) &&
                        prior->eye == uint32_t(eye) &&
                        prior->bytes->data() == old[eye]->bytes->data()));
        if (!intact[eye])
          throw std::runtime_error(
              "old per-eye packet metadata/reference changed during backend");
        double waited = deferred ? tx.wait_idle(mode, eye) : 0.0;
        if (prior != previous[mode][eye])
          throw std::runtime_error(
              "old per-eye packet replaced before sender wait completed");
        auto sent = std::make_shared<TxPacket>();
        sent->frame = uint64_t(frame);
        sent->eye = uint32_t(eye);
        sent->bytes = packet;
        std::shared_ptr<const TxPacket> immutable = sent;
        previous[mode][eye] = immutable;
        tx.send(mode, immutable);
        return waited;
      };
      Sample s = run(a, z0, z1, true, before, after);
      bool unchanged = intact[0] && intact[1];
      if (!unchanged)
        throw std::runtime_error("old packet changed");
      if (measured) {
        Row r;
        r.order = order++;
        r.mode = mode;
        r.frame = frame;
        r.cycle = s.wall;
        r.record = s.record;
        r.submit = s.submit;
        r.before0 = s.senderBefore[0];
        r.before1 = s.senderBefore[1];
        r.after0 = s.senderAfter[0];
        r.after1 = s.senderAfter[1];
        r.backend0 = s.backend0;
        r.backend1 = s.backend1;
        r.fence0 = s.fence0;
        r.fence1 = s.fence1;
        r.zstd0 = s.zstd0;
        r.zstd1 = s.zstd1;
        r.pack0 = s.pack0;
        r.pack1 = s.pack1;
        r.gpu0 = s.gpu0;
        r.gpu1 = s.gpu1;
        r.gpuRead0 = s.gpuRead0;
        r.gpuRead1 = s.gpuRead1;
        r.p0 = s.packet[0]->size();
        r.p1 = s.packet[1]->size();
        r.intact = unchanged;
        r.raw = {std::move(s.raw0), std::move(s.raw1)};
        r.packet = std::move(s.packet);
        batchRows.push_back(std::move(r));
      }
      frameNo[mode]++;
    };
    auto validate = [&](std::vector<Row> &batch) {
      for (auto &r : batch) {
        for (int e = 0; e < 2; e++) {
          if (!verify_packet(*r.packet[e], r.raw[e]))
            throw std::runtime_error(
                "packet does not decompress to ASTC bytes");
          if (rawBaseline[r.mode][e].empty()) {
            rawBaseline[r.mode][e] = r.raw[e];
            packetBaseline[r.mode][e] = *r.packet[e];
          } else if (rawBaseline[r.mode][e] != r.raw[e] ||
                     packetBaseline[r.mode][e] != *r.packet[e])
            throw std::runtime_error("ASTC or packet output changed");
          r.raw[e].clear();
          r.packet[e].reset();
        }
        rows.push_back(std::move(r));
      }
    };
    // Both eye dispatches are submitted inside run() before per-eye callbacks
    // can wait.
    for (int mode = 0; mode < modes; mode++) {
      tx.set_rate(mode, bitrate[mode]);
      std::vector<Row> warmRows;
      for (int i = 0; i < warm; i++)
        step(mode, false, warmRows);
      tx.wait_all();
    }
    for (int block = 0; block < count / batch; block++) {
      std::array<int, 4> ord = block % 2 == 0 ? std::array<int, 4>{0, 1, 3, 2}
                                              : std::array<int, 4>{2, 3, 1, 0};
      for (int k = 0; k < 5; k++) {
        int mode = k == 4 ? 4 : ord[k];
        tx.set_rate(mode, bitrate[mode]);
        std::vector<Row> batchRows;
        batchRows.reserve(batch);
        for (int i = 0; i < batch; i++)
          step(mode, true, batchRows);
        tx.wait_all();
        validate(batchRows);
      }
    }
    for (int m = 1; m < modes; m++)
      if (rawBaseline[0] != rawBaseline[m] ||
          packetBaseline[0] != packetBaseline[m])
        throw std::runtime_error("all-treatment ASTC/packet bytes differ");
    auto report = [&](int mode, auto value) {
      std::vector<double> v;
      for (auto &r : rows)
        if (r.mode == mode)
          v.push_back(value(r));
      return std::pair{pct(v, .5), pct(v, .95)};
    };
    auto label = [&](const char *n, int m, auto f) {
      auto [x, y] = report(m, f);
      std::cout << n << "=" << x << "/" << y << "ms ";
    };
    for (int m = 0; m < modes; m++) {
      std::cout << names[m] << ' ';
      label("cycle", m, [](auto &r) { return r.cycle; });
      label("backend_eye0", m, [](auto &r) { return r.backend0; });
      label("backend_eye1", m, [](auto &r) { return r.backend1; });
      label("wait_before_sum", m,
            [](auto &r) { return r.before0 + r.before1; });
      label("wait_after_sum", m, [](auto &r) { return r.after0 + r.after1; });
      label("gpu0", m, [](auto &r) { return r.gpu0; });
      label("gpu1", m, [](auto &r) { return r.gpu1; });
      std::cout << '\n';
    }
    for (auto &r : rows)
      csv << r.order << ',' << names[r.mode] << ',' << r.frame << ','
          << bitrate[r.mode] << ',' << r.cycle << ',' << r.record << ','
          << r.submit << ',' << r.before0 << ',' << r.before1 << ',' << r.after0
          << ',' << r.after1 << ',' << r.backend0 << ',' << r.backend1 << ','
          << r.fence0 << ',' << r.fence1 << ',' << r.zstd0 << ',' << r.zstd1
          << ',' << r.pack0 << ',' << r.pack1 << ',' << r.gpu0 << ',' << r.gpu1
          << ',' << r.gpuRead0 << ',' << r.gpuRead1 << ',' << r.p0 << ','
          << r.p1 << ',' << r.intact << '\n';
    auto writeastc = [&](const std::string &p,
                         const std::vector<uint8_t> &raw) {
      uint8_t h[16] = {0x13, 0xab, 0xa1, 0x5c, 8, 8, 1, 0x80,
                       0x08, 0,    0x80, 0x08, 0, 1, 0, 0};
      std::ofstream f(p, std::ios::binary);
      f.write((char *)h, 16);
      f.write((char *)raw.data(), raw.size());
      if (!f)
        throw std::runtime_error("write astc");
    };
    writeastc(out + "sender-eye0.astc", rawBaseline[0][0]);
    writeastc(out + "sender-eye1.astc", rawBaseline[0][1]);
    std::cout << "device=" << props.deviceName << " warm=" << warm
              << " measured=" << count
              << " per_condition both_submitted_before_any_wait "
                 "one_fifo_sender_per_eye_idle immutable_old_packet_checked "
                 "all_outputs_bitexact=true\n";
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
