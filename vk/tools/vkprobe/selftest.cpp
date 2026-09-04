// selftest.cpp - nxvc-vkprobe --selftest.
//
// The probe answers "may this device run the codec".  This answers the other
// half: "does the runtime layer actually work on it".  It exercises, on a real
// device, every helper the encoder and decoder will lean on -- buffers of each
// kind, a storage image and its layout transition, a one-shot submit, a
// pre-recorded command buffer, a timeline semaphore signalled from the host and
// from the queue, and a timestamp query pool around a real dispatch.
//
// It is deliberately not a benchmark and not a correctness test of any codec
// pass: it is the smoke test that tells a downstream agent whether a failure is
// theirs or the runtime's.
#include <nxvc/vk/vk_common.hpp>

#include <nxvc/vk/shaders/nxvc_subgroup_semantics.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Step {
    std::string name;
    bool ok = false;
    std::string detail;
};

struct Push {
    uint32_t element_count;
};

}  // namespace

int nxvcVkSelfTest(bool prefer_software, uint32_t device_index) {
    using namespace nxvc::vk;

    std::vector<Step> steps;
    const auto step = [&](const char* name, auto&& fn) {
        Step s;
        s.name = name;
        try {
            s.detail = fn();
            s.ok = true;
        } catch (const std::exception& e) {
            s.detail = e.what();
            s.ok = false;
        }
        steps.push_back(std::move(s));
        return steps.back().ok;
    };

    std::unique_ptr<Context> ctx;
    try {
        ContextCreateInfo ci;
        ci.app_name = "nxvc-vkprobe --selftest";
        ci.prefer_software = prefer_software;
        ci.device_index = device_index;
        ci.allow_hybrid = true;
        ctx = Context::create(ci);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nxvc-vkprobe --selftest: skipping: %s\n", e.what());
        return 77;
    }
    const Probe& p = ctx->probe();

    // ------------------------------------------------------------ buffers
    step("buffer.device_local", [&] {
        Buffer b(*ctx, 4u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 BufferKind::DeviceLocal, "selftest.device_local");
        return "4 MiB";
    });

    // The 3.6 send ring.  Not having a cached heap is a *note*, not a failure:
    // the fallback is staging through device-local.
    step("buffer.host_cached", [&] {
        Buffer b(*ctx, 1u << 20,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 BufferKind::HostCached, "selftest.send_ring");
        if (!b.mapped()) throw Error(NXVC_VK_ERR_INTERNAL, "host-visible but unmapped");
        std::memset(b.mapped(), 0xA5, 4096);
        b.flush(0, 4096);
        return b.cached() ? std::string("cached, mapped")
                          : std::string("mapped but NOT cached; stage through "
                                        "device-local per 3.6");
    });

    step("buffer.readback_roundtrip", [&] {
        constexpr uint32_t kN = 1024;
        Buffer up(*ctx, kN * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  BufferKind::HostUpload);
        Buffer dev(*ctx, kN * 4,
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   BufferKind::DeviceLocal);
        Buffer back(*ctx, kN * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    BufferKind::HostReadback);
        auto src = up.span<uint32_t>();
        for (uint32_t i = 0; i < kN; ++i) src[i] = i * 2654435761u;
        up.flush();

        immediate(*ctx, [&](VkCommandBuffer cmd) {
            VkBufferCopy c{0, 0, kN * 4};
            vkCmdCopyBuffer(cmd, up.handle(), dev.handle(), 1, &c);
            const auto bar = dev.barrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                                         VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                                 &bar, 0, nullptr);
            vkCmdCopyBuffer(cmd, dev.handle(), back.handle(), 1, &c);
        });
        back.invalidate();
        auto got = back.span<uint32_t>();
        for (uint32_t i = 0; i < kN; ++i)
            if (got[i] != src[i])
                throw Error(NXVC_VK_ERR_INTERNAL,
                            "readback mismatch at " + std::to_string(i));
        return "4 KiB host -> device -> host, byte-identical";
    });

    // ------------------------------------------------------------- images
    step("image.storage_sampled", [&] {
        ImageDesc d;
        d.width = 256;
        d.height = 256;
        d.layers = 2;  // a stereo pair
        d.format = VK_FORMAT_R8G8B8A8_UNORM;
        d.debug_name = "selftest.reference";
        Image img(*ctx, d);
        immediate(*ctx, [&](VkCommandBuffer cmd) {
            img.transition(cmd, VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT);
        });
        if (img.layout() != VK_IMAGE_LAYOUT_GENERAL)
            throw Error(NXVC_VK_ERR_INTERNAL, "layout not tracked");
        return "256x256x2 RGBA8 storage+sampled, transitioned to GENERAL";
    });

    // -------------------------------------------------------- timeline sem
    if (p.caps & NXVC_VK_CAP_TIMELINE_SEMAPHORE) {
        step("semaphore.timeline", [&] {
            TimelineSemaphore t(*ctx, 0);
            if (t.value() != 0) throw Error(NXVC_VK_ERR_INTERNAL, "initial value");
            t.signalHost(5);
            if (!t.wait(5, 1'000'000'000ull))
                throw Error(NXVC_VK_ERR_INTERNAL, "host signal not observed");
            // Queue-side: an empty submit that waits on 5 and signals 9, the
            // shape of the 3.6 row-group handoff.
            t.submit(VK_NULL_HANDLE, 5, 9);
            if (!t.wait(9, 5'000'000'000ull))
                throw Error(NXVC_VK_ERR_INTERNAL, "queue signal not observed");
            return "host signal 5, queue wait 5 / signal 9";
        });
    }

    // ------------------------------------------- pipeline, dispatch, timing
    step("pipeline.dispatch_and_timing", [&] {
        constexpr uint32_t kElements = 4096;
        constexpr uint32_t kWg = 256;

        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{
            DescriptorSetLayout::b(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            DescriptorSetLayout::b(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
        DescriptorSetLayout layout(*ctx, bindings);
        const std::array<VkDescriptorPoolSize, 1> pool{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
        DescriptorSet set(*ctx, layout.handle(), pool);

        Buffer out(*ctx, kElements * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   BufferKind::HostReadback, "selftest.out");
        Buffer status(*ctx, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      BufferKind::HostReadback, "selftest.status");
        std::memset(status.mapped(), 0, status.size());
        set.write(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out.descriptor())
            .write(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, status.descriptor());
        set.flush();

        const std::array<VkDescriptorSetLayout, 1> layouts{layout.handle()};
        ComputePipelineDesc desc;
        desc.spirv = shaders::nxvc_subgroup_semantics_spv;
        desc.set_layouts = layouts;
        desc.push_constant_size = sizeof(Push);
        desc.spec.set(kSpecWorkgroupSize, kWg);
        desc.debug_name = "selftest";
        ComputePipeline pipe(*ctx, desc);

        TimestampPool timing(*ctx, {"selftest_dispatch"}, /*slots=*/2);
        PersistentCommands cmds(*ctx, 2);
        const VkDescriptorSet vk_set = set.handle();

        cmds.record(0, [&](VkCommandBuffer cmd) {
            timing.reset(cmd, 0);
            TimestampPool::Scope scope(timing, cmd, 0, 0);
            pipe.bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.layout(),
                                    0, 1, &vk_set, 0, nullptr);
            const Push pc{kElements};
            pipe.push(cmd, pc);
            vkCmdDispatch(cmd, divRoundUp(kElements, kWg), 1, 1);
        });

        VkCommandBuffer cb = cmds.at(0);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        NXVC_VK_CHECK(vkQueueSubmit(ctx->queue(), 1, &si, VK_NULL_HANDLE));
        ctx->waitIdle();

        status.invalidate();
        const uint32_t observed = static_cast<const uint32_t*>(status.mapped())[2];
        if (observed == 0)
            throw Error(NXVC_VK_ERR_INTERNAL, "shader did not run");
        if (observed != pipe.subgroupSize())
            throw Error(NXVC_VK_ERR_INTERNAL,
                        "pipeline reports subgroup size " +
                            std::to_string(pipe.subgroupSize()) + " but the shader "
                            "saw " + std::to_string(observed));

        std::string detail = "subgroup size " + std::to_string(observed);
        if (auto rep = timing.read(0, /*wait=*/true)) {
            char b[96];
            std::snprintf(b, sizeof b, ", dispatch %.3f ms", rep->total_ms);
            detail += b;
        } else {
            detail += ", no timestamps";
        }
        return detail;
    });

    // -------------------------------------------------------------- adopt
    // The WiVRn / Monado path of 3.6: hand the library a device someone else
    // owns.  Adopting our own handles is a faithful rehearsal -- the adopted
    // Context must resolve its own function pointers, re-derive the timestamp
    // bits for the host's queue family, and destroy nothing on the way out.
    step("context.adopt", [&] {
        std::vector<std::string> enabled;  // what we know we enabled
        AdoptInfo ai;
        ai.instance = ctx->instance();
        ai.physical_device = ctx->physicalDevice();
        ai.device = ctx->device();
        ai.queue = ctx->queue();
        ai.queue_family = ctx->queueFamily();
        ai.api_version = ctx->apiVersion();
        auto borrowed = Context::adopt(ai);
        if (!borrowed->adopted())
            throw Error(NXVC_VK_ERR_INTERNAL, "adopted flag not set");
        if (borrowed->device() != ctx->device())
            throw Error(NXVC_VK_ERR_INTERNAL, "device handle not carried over");
        if (borrowed->probe().profile != p.profile)
            throw Error(NXVC_VK_ERR_INTERNAL, "profile differs after adoption");
        // Allocate and submit through the borrowed context, then let it die:
        // if it destroyed the host's device the steps after this one would
        // fail, and the process would very likely not survive teardown.
        Buffer b(*borrowed, 64 * 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 BufferKind::HostUpload, "adopted.buffer");
        immediate(*borrowed, [&](VkCommandBuffer cmd) {
            vkCmdFillBuffer(cmd, b.handle(), 0, VK_WHOLE_SIZE, 0x5A5A5A5Au);
        });
        b.invalidate();
        if (b.span<uint32_t>()[0] != 0x5A5A5A5Au)
            throw Error(NXVC_VK_ERR_INTERNAL, "fill through adopted queue failed");
        return std::string("allocated and submitted on a borrowed VkDevice, "
                           "queue family " + std::to_string(borrowed->queueFamily()));
    });

    // The host device must still be alive after the adopted context died.
    step("context.adopt_left_device_intact", [&] {
        Buffer b(*ctx, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferKind::HostUpload);
        ctx->waitIdle();
        return "owner still usable";
    });

    // ----------------------------------------------------------- external
    step("external.support", [&] {
        return externalSupport(*ctx).toString();
    });

    // ------------------------------------------------------------- report
    bool all_ok = true;
    std::printf("{\n  \"device\": \"%s\",\n", p.device_name);
    std::printf("  \"profile\": \"%s\",\n", nxvc_vk_profile_string(p.profile));

    std::printf("  \"steps\": [\n");
    for (size_t i = 0; i < steps.size(); ++i) {
        all_ok = all_ok && steps[i].ok;
        std::printf("    {\"name\": \"%s\", \"ok\": %s, \"detail\": \"%s\"}%s\n",
                    steps[i].name.c_str(), steps[i].ok ? "true" : "false",
                    steps[i].detail.c_str(), i + 1 < steps.size() ? "," : "");
    }
    std::printf("  ],\n  \"pass\": %s\n}\n", all_ok ? "true" : "false");
    return all_ok ? 0 : 1;
}
