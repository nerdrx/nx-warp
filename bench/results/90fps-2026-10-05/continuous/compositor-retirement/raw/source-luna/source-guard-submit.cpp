#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <stdexcept>

static int checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL:%d %s\n",__LINE__,#x); } } while(false)
constexpr int XRT_SUCCESS=0;
struct Rendering {};
struct Frame { Rendering rendering; };
struct Pool { int resets=0; void reset() { ++resets; } };
struct Fence { int id=77; };
struct FenceHandle { Fence value; Fence &operator*() { return value; } };
struct SemaphoreHandle { int value=9; int operator*() const { return value; } };
namespace vk {
enum class Result { eSuccess, eTimeout, eError };
enum class PipelineStageFlagBits2 { eComputeShader };
struct SemaphoreSubmitInfo { int semaphore; uint64_t value; PipelineStageFlagBits2 stageMask; };
struct CommandBufferSubmitInfo { int commandBuffer; };
struct SubmitInfo2 {
    uint32_t commandBufferInfoCount; const CommandBufferSubmitInfo *pCommandBufferInfos;
    uint32_t signalSemaphoreInfoCount; const SemaphoreSubmitInfo *pSignalSemaphoreInfos;
};
}
struct FakeDevice {
    int waits=0, resets=0; vk::Result wait_result=vk::Result::eSuccess; bool wait_throws=false, reset_throws=false; uint64_t wait_timeout=99;
    vk::Result waitForFences(Fence &, bool, uint64_t timeout) { ++waits; wait_timeout=timeout; if(wait_throws) throw std::runtime_error("fence status error"); return wait_result; }
    void resetFences(Fence &) { ++resets; if(reset_throws) throw std::runtime_error("reset error"); }
};
struct FakeQueue {
    std::mutex mutex; int calls=0, fence_id=0, signal_semaphore=0; uint64_t signal_value=0;
    vk::PipelineStageFlagBits2 signal_stage=vk::PipelineStageFlagBits2::eComputeShader; bool submit_throws=false;
    void submit2(const vk::SubmitInfo2 &info, Fence &fence) {
        ++calls;
        if(submit_throws) throw std::runtime_error("submit error");
        fence_id=fence.id;
        signal_semaphore=info.pSignalSemaphoreInfos[0].semaphore;
        signal_value=info.pSignalSemaphoreInfos[0].value;
        signal_stage=info.pSignalSemaphoreInfos[0].stageMask;
    }
};
struct QueueHolder { std::mutex mutex; FakeQueue queue; };
struct FakeVk { FakeDevice device; QueueHolder queue; };
struct Harness {
    FakeVk vk; FenceHandle submission_fence; SemaphoreHandle sem; int cmd=12; uint64_t sem_value=5;
    bool submission_pending=false, motion_unsafe=false, check_acquire_state=false;
    Frame frame; Pool cmd_pool; int clears=0, acquires=0, acquire_result=4;
    void comp_frame_clear_locked(Rendering *r) { CHECK(r==&frame.rendering); ++clears; }
    int acquire_image() { ++acquires; if(check_acquire_state) CHECK(!submission_pending && !motion_unsafe); return acquire_result; }
    int guard_then_pool_reset() {
    if (submission_pending)
    {
        if (vk.device.waitForFences(*submission_fence, true, 0) == vk::Result::eTimeout)
        {
            comp_frame_clear_locked(&frame.rendering);
            return XRT_SUCCESS;
        }
        submission_pending = false;
        motion_unsafe = false;
    }
    int i = acquire_image();
    if (i < 0)
    {
        comp_frame_clear_locked(&frame.rendering);
        return XRT_SUCCESS;
    }
    cmd_pool.reset();
    return i;

    }
    void submit() {
            const vk::SemaphoreSubmitInfo sem_info{
            .semaphore = *sem,
            .value = sem_value + 1,
            .stageMask = vk::PipelineStageFlagBits2::eComputeShader,
    };
{
        vk::CommandBufferSubmitInfo cmd_info{
                .commandBuffer = cmd,
        };
        std::unique_lock lock{vk.queue.mutex};
        vk.device.resetFences(*submission_fence);
        vk.queue.queue.submit2(vk::SubmitInfo2{
                .commandBufferInfoCount = 1,
                .pCommandBufferInfos = &cmd_info,
                .signalSemaphoreInfoCount = 1,
                .pSignalSemaphoreInfos = &sem_info,
        }, *submission_fence);
        sem_value = sem_info.value;
        submission_pending = true;
    }
    }
    void compute_success_state_update() { motion_unsafe = false; }
};
void initial_and_timeout() {
    Harness h;
    CHECK(h.guard_then_pool_reset()==4);
    CHECK(h.vk.device.waits==0 && h.acquires==1 && h.cmd_pool.resets==1);
    Harness t; t.submission_pending=true; t.motion_unsafe=true; t.vk.device.wait_result=vk::Result::eTimeout;
    CHECK(t.guard_then_pool_reset()==XRT_SUCCESS);
    CHECK(t.vk.device.waits==1 && t.vk.device.wait_timeout==0);
    CHECK(t.clears==1 && t.acquires==0 && t.cmd_pool.resets==0);
    CHECK(t.submission_pending && t.motion_unsafe);
}
void healthy_fence_wait_and_error() {
    Harness h; h.submission_pending=true; h.motion_unsafe=true; h.check_acquire_state=true;
    CHECK(h.guard_then_pool_reset()==4);
    CHECK(!h.submission_pending && !h.motion_unsafe && h.acquires==1 && h.cmd_pool.resets==1);
    Harness e; e.submission_pending=true; e.motion_unsafe=true; e.vk.device.wait_throws=true;
    bool threw=false; try { (void)e.guard_then_pool_reset(); } catch(const std::exception &) { threw=true; }
    CHECK(threw && e.submission_pending && e.motion_unsafe && e.clears==0 && e.acquires==0 && e.cmd_pool.resets==0);
}
void acquire_failure_after_retirement() {
    Harness h; h.submission_pending=true; h.motion_unsafe=true; h.acquire_result=-1;
    CHECK(h.guard_then_pool_reset()==XRT_SUCCESS);
    CHECK(h.vk.device.waits==1 && !h.submission_pending && !h.motion_unsafe);
    CHECK(h.acquires==1 && h.clears==1 && h.cmd_pool.resets==0);
}
void submit_publication_failures() {
    Harness reset_fail; reset_fail.vk.device.reset_throws=true;
    bool reset_threw=false; try { reset_fail.submit(); } catch(const std::exception &) { reset_threw=true; }
    CHECK(reset_threw && reset_fail.vk.device.resets==1 && reset_fail.vk.queue.queue.calls==0);
    CHECK(reset_fail.sem_value==5 && !reset_fail.submission_pending);

    Harness submit_fail; submit_fail.vk.queue.queue.submit_throws=true;
    bool submit_threw=false; try { submit_fail.submit(); } catch(const std::exception &) { submit_threw=true; }
    CHECK(submit_threw && submit_fail.vk.device.resets==1 && submit_fail.vk.queue.queue.calls==1);
    CHECK(submit_fail.sem_value==5 && !submit_fail.submission_pending);
}
void successful_submit_and_compute_signal() {
    Harness h; h.submit();
    CHECK(h.vk.device.resets==1 && h.vk.queue.queue.calls==1);
    CHECK(h.sem_value==6 && h.submission_pending);
    CHECK(h.vk.queue.queue.signal_semaphore==9 && h.vk.queue.queue.signal_value==6);
    CHECK(h.vk.queue.queue.fence_id==77);
    CHECK(h.vk.queue.queue.signal_stage==vk::PipelineStageFlagBits2::eComputeShader);
    h.compute_success_state_update();
    CHECK(h.motion_unsafe==false && h.submission_pending);
}
void submit_timeout_late_success_resubmit_cycle() {
    Harness h;
    h.submit();
    CHECK(h.sem_value==6 && h.submission_pending && h.vk.queue.queue.calls==1);

    h.vk.device.wait_result=vk::Result::eTimeout;
    const int acquire_count=h.acquires, pool_reset_count=h.cmd_pool.resets;
    CHECK(h.guard_then_pool_reset()==XRT_SUCCESS);
    CHECK(h.submission_pending && h.sem_value==6 && h.vk.queue.queue.calls==1);
    CHECK(h.acquires==acquire_count && h.cmd_pool.resets==pool_reset_count);

    h.vk.device.wait_result=vk::Result::eSuccess;
    h.check_acquire_state=true;
    CHECK(h.guard_then_pool_reset()==4);
    CHECK(!h.submission_pending && !h.motion_unsafe && h.acquires==acquire_count+1);
    CHECK(h.cmd_pool.resets==pool_reset_count+1);

    h.submit();
    CHECK(h.sem_value==7 && h.submission_pending && h.vk.queue.queue.calls==2);
    CHECK(h.vk.device.resets==2 && h.vk.queue.queue.fence_id==77);
    CHECK(h.vk.queue.queue.signal_value==7);
}
int main() {
    initial_and_timeout(); healthy_fence_wait_and_error();
    acquire_failure_after_retirement(); submit_publication_failures();
    successful_submit_and_compute_signal(); submit_timeout_late_success_resubmit_cycle();
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
