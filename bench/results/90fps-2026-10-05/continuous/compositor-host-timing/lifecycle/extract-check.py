#!/usr/bin/env python3
import hashlib
import pathlib
import sys

repo = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
source = repo / "server/compositor/compositor.cpp"
text = source.read_text()
header = (repo / "server/compositor/compositor.h").read_text()
host_start = text.index("\tconst bool host_timing = session.dump_timings_enabled();", text.index("xrt_result_t compositor::layer_commit"))
host_end = text.index("\n\t};", text.index("auto host_dump =", host_start)) + len("\n\t};")
host_block = text[host_start:host_end].replace(
    'std::format(",{},{}", host_times[2 * stage], outcome).c_str()',
    'fake_format(host_times[2 * stage], outcome).c_str()')
if "vk::raii::Fence submission_fence;" not in header or "bool submission_pending = false;" not in header:
    raise SystemExit("submission fields changed")
constructor = text[text.index("compositor::compositor(wivrn_session & session)"):]
if "submission_fence{vk.device, vk::FenceCreateInfo{}}" not in constructor:
    raise SystemExit("submission fence constructor initializer changed")

def balanced_block(start):
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{": depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0: return text[start:pos + 1], pos + 1
    raise SystemExit("unclosed source block")

layer = text.index("xrt_result_t compositor::layer_commit")
guard_start = text.index("\tif (submission_pending)", layer)
guard, guard_end = balanced_block(guard_start)
acquire_start = text.index("\tint i = acquire_image();", guard_end)
acquire_line = text[acquire_start:text.index("\n", acquire_start)]
no_image_start = text.index("\tif (i < 0)", acquire_start)
no_image, no_image_end = balanced_block(no_image_start)
pool_reset_start = text.index("\tcmd_pool.reset();", no_image_end)
pool_reset = text[pool_reset_start:text.index("\n", pool_reset_start)]
if not (guard_start < acquire_start < no_image_start < pool_reset_start):
    raise SystemExit("unexpected pre-acquire/reset source ordering")

sem_start = text.index("\tconst vk::SemaphoreSubmitInfo sem_info{", layer)
sem_end = text.index("\n\t};", sem_start) + len("\n\t};")
sem_decl = text[sem_start:sem_end]
submit_start = text.index("\n\t{\n\t\tvk::CommandBufferSubmitInfo cmd_info{", sem_end) + 2
submit_block, submit_end = balanced_block(submit_start)
for needle in ("vk.device.resetFences(*submission_fence);", "vk.queue.queue.submit2(", "sem_value = sem_info.value;", "submission_pending = true;"):
    if needle not in submit_block: raise SystemExit(f"missing submit source element: {needle}")
compute_wait = text.index("const auto timeline = vk.device.waitSemaphores", submit_end)
timeout_marker = text.index("== vk::Result::eTimeout)", compute_wait)
timeout_body = text.index("{", timeout_marker)
_, timeout_end = balanced_block(timeout_body)
else_start = text.index("else", timeout_end)
compute_else, compute_end = balanced_block(else_start)
compute_path = text[compute_wait:compute_end]
if "submission_pending" in compute_path:
    raise SystemExit("compute-wait path unexpectedly mutates submission_pending")
motion_clear_start = text.index("motion_unsafe = false;", compute_wait)
motion_clear = text[motion_clear_start:text.index("\n", motion_clear_start)]

prefix = r'''#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>

static int checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL:%d %s\n",__LINE__,#x); } } while(false)
constexpr int XRT_SUCCESS=0;
struct Rendering { uint64_t id=3; };
struct Frame { Rendering rendering; };
struct Pool { int resets=0; void reset() { ++resets; } };
struct FakeSession {
    bool dump_timings_enabled() const { return false; }
    void dump_time(const char *, uint64_t, int64_t, uint8_t, const char *) {}
};
static int64_t os_monotonic_get_ns() { return 1; }
static std::string fake_format(int64_t start, const char * outcome) { return ","+std::to_string(start)+","+outcome; }
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
    FakeVk vk; FakeSession session; FenceHandle submission_fence; SemaphoreHandle sem; int cmd=12; uint64_t sem_value=5;
    bool submission_pending=false, motion_unsafe=false, check_acquire_state=false;
    Frame frame; Pool cmd_pool; int clears=0, acquires=0, acquire_result=4;
    void comp_frame_clear_locked(Rendering *r) { CHECK(r==&frame.rendering); ++clears; }
    int acquire_image() { ++acquires; if(check_acquire_state) CHECK(!submission_pending && !motion_unsafe); return acquire_result; }
    int guard_then_pool_reset() {
'''
source_guard = host_block + "\n" + guard + "\n" + acquire_line + "\n" + no_image + "\n" + pool_reset + "\n\treturn i;"
raw_submit = sem_decl + "\n" + submit_block
source_submit = host_block + "\n(void)host_frame_id; (void)host_dump;\n" + raw_submit
suffix = r'''
    }
    void submit() {
        __SUBMIT_SOURCE__
    }
    void compute_success_state_update() { __MOTION_CLEAR_SOURCE__ }
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
'''
source_submit = host_block + "\n(void)host_frame_id; (void)host_dump;\n" + raw_submit.replace("\t", "    ")
suffix = suffix.replace("__SUBMIT_SOURCE__", source_submit).replace("__MOTION_CLEAR_SOURCE__", motion_clear)
generated = prefix + source_guard.replace("\t", "    ") + "\n" + suffix
(out / "source-guard-submit.cpp").write_text(generated)
(out / "provenance.txt").write_text(
    f"source={source}\nsource_sha256={hashlib.sha256(source.read_bytes()).hexdigest()}\n"
    f"guard_sha256={hashlib.sha256(guard.encode()).hexdigest()}\n"
    f"submit_sha256={hashlib.sha256(raw_submit.encode()).hexdigest()}\n"
    f"compute_success_assignment_sha256={hashlib.sha256(motion_clear.encode()).hexdigest()}\n"
    f"compute_path_sha256={hashlib.sha256(compute_path.encode()).hexdigest()}\n"
    f"header_sha256={hashlib.sha256(header.encode()).hexdigest()}\n"
    f"constructor_has_submission_fence_initializer=true\n"
    f"guard_lines={text[:guard_start].count(chr(10))+1}-{text[:guard_end].count(chr(10))+1}\n"
    f"submit_lines={text[:sem_start].count(chr(10))+1}-{text[:submit_end].count(chr(10))+1}\n"
    f"compute_success_assignment_line={text[:motion_clear_start].count(chr(10))+1}\n"
    f"compute_path_lines={text[:compute_wait].count(chr(10))+1}-{text[:compute_end].count(chr(10))+1}\n"
    f"ordering=guard<{acquire_start}<cmd_pool.reset at {pool_reset_start}\n"
)
