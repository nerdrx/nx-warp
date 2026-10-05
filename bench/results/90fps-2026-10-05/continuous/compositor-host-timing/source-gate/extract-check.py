#!/usr/bin/env python3
import hashlib
import pathlib
import sys

repo = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
src = repo / "server/compositor/compositor.cpp"
text = src.read_text()
start = text.index("\tconst bool host_timing = session.dump_timings_enabled();", text.index("xrt_result_t compositor::layer_commit"))
end = text.index("\n\t};", text.index("auto host_dump =", start)) + len("\n\t};")
block = text[start:end]
block = block.replace(
    'std::format(",{},{}", host_times[2 * stage], outcome).c_str()',
    'fake_format(host_times[2 * stage], outcome).c_str()')
if "std::format" in block:
    raise SystemExit("format adapter replacement failed")

layer = text.index("xrt_result_t compositor::layer_commit")
body = text[layer: text.index("\nvoid compositor::drop_retained_frame", layer)]
order_checks = [
    ("host_stamp(0);", "vk.device.waitForFences(*submission_fence, true, 0)", "host_stamp(1);"),
    ("host_stamp(2);", "int i = acquire_image();", "host_stamp(3);"),
    ("host_stamp(4);", "cmd_pool.reset();", "cmd.end();"),
    ("cmd.end();", "host_stamp(5);", "const vk::SemaphoreSubmitInfo sem_info"),
    ("host_stamp(6);", "std::unique_lock lock{vk.queue.mutex};", "host_stamp(7);"),
    ("host_stamp(7);", "host_stamp(8);", "vk.device.resetFences(*submission_fence);"),
    ("vk.queue.queue.submit2(", "submission_pending = true;", "host_stamp(9);"),
    ("host_stamp(10);", "for (auto & encoder: encoders)", "host_stamp(11);"),
    ("host_stamp(12);", "const auto timeline = vk.device.waitSemaphores", "host_stamp(13);"),
    ("host_stamp(13);", "host_stamp(14);", "auto [res, ts] = query_pool.getResults<uint64_t>("),
    ("auto [res, ts] = query_pool.getResults<uint64_t>(", "host_stamp(15);", "if (res == vk::Result::eSuccess)"),
    ("host_stamp(16);", "comp_swapchain_shared_garbage_collect(&cscs);", "host_stamp(17);"),
]
for a, b, c in order_checks:
    ia = body.find(a)
    ib = body.find(b, ia + len(a)) if ia >= 0 else -1
    ic = body.find(c, ib + len(b)) if ib >= 0 else -1
    if min(ia, ib, ic) < 0:
        raise SystemExit(f"stage boundary ordering failed: {a} < {b} < {c}")

if body.count("host_dump(") != 3:  # timeout, no-image, normal flush
    raise SystemExit("unexpected number of flush sites")
if body.index("host_dump(info.frame_id") < body.index("comp_swapchain_shared_garbage_collect(&cscs);"):
    raise SystemExit("normal flush must follow garbage collection")
for a, b, c, d in [
    ("if (retirement == vk::Result::eTimeout)", 'host_dump(host_frame_id, "retirement_timeout");', "comp_frame_clear_locked(&frame.rendering);", "return XRT_SUCCESS;"),
    ("if (i < 0)", 'host_dump(host_frame_id, "no_image");', "comp_frame_clear_locked(&frame.rendering);", "return XRT_SUCCESS;"),
    ("comp_swapchain_shared_garbage_collect(&cscs);", 'host_dump(info.frame_id, timeline == vk::Result::eTimeout ? "timeline_timeout" : "submitted");', "return XRT_SUCCESS;", "\n}")]:
    pos = -1
    for part in (a,b,c,d):
        pos = body.find(part, pos + 1)
        if pos < 0:
            raise SystemExit(f"flush/return order failed: {a} < {b} < {c} < {d}")

h = hashlib.sha256(src.read_bytes()).hexdigest()
(out / "host-stage-source.cpp").write_text(r'''#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
static int checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL:%d %s\n",__LINE__,#x); } } while(false)
static int clock_calls=0, format_calls=0;
int64_t os_monotonic_get_ns() { ++clock_calls; return int64_t(clock_calls) * 100; }
std::string fake_format(int64_t start, const char * outcome) { ++format_calls; return ","+std::to_string(start)+","+outcome; }
struct Row { std::string name, extra; uint64_t frame=0; int64_t time=0; uint8_t stream=0; };
struct FakeSession {
    bool enabled=false; std::vector<Row> rows;
    bool dump_timings_enabled() const { return enabled; }
    void dump_time(const char * name, uint64_t frame, int64_t time, uint8_t stream, const char * extra) {
        rows.push_back({name, extra, frame, time, stream});
    }
};
struct FakeFrame { struct { uint64_t id=13; } rendering; };
int64_t extra_start(const std::string & extra) { auto comma=extra.find(',',1); return std::stoll(extra.substr(1,comma-1)); }
void run_disabled() {
    FakeSession session; FakeFrame frame;
    __BLOCK__
    host_stamp(0); host_stamp(1); host_dump(99, "submitted");
    (void)host_frame_id;
    CHECK(clock_calls==0 && format_calls==0 && session.rows.empty());
}
void run_unpaired_and_early_outcome() {
    FakeSession session; session.enabled=true; FakeFrame frame;
    __BLOCK__
    host_stamp(2); host_stamp(3); host_stamp(4); // incomplete record stage
    host_dump(host_frame_id, "no_image");
    CHECK(clock_calls==3 && format_calls==1 && session.rows.size()==1);
    CHECK(session.rows[0].name=="compositor_acquire" && session.rows[0].frame==13);
    CHECK(session.rows[0].stream==255 && session.rows[0].time>extra_start(session.rows[0].extra));
    CHECK(session.rows[0].extra.find(",no_image")!=std::string::npos);
}
void run_all_nine() {
    FakeSession session; session.enabled=true; FakeFrame frame;
    __BLOCK__
    for (size_t i=0; i<host_stage_names.size(); ++i) { host_stamp(2*i); host_stamp(2*i+1); }
    host_dump(777, "timeline_timeout");
    CHECK(host_stage_names.size()==9 && session.rows.size()==9 && host_frame_id==13);
    CHECK(clock_calls==21 && format_calls==10);
    for (size_t i=0; i<session.rows.size(); ++i) {
        CHECK(session.rows[i].name==host_stage_names[i]);
        CHECK(session.rows[i].frame==777 && session.rows[i].time>extra_start(session.rows[i].extra));
        CHECK(session.rows[i].extra.find(",timeline_timeout")!=std::string::npos);
    }
}
int main() {
    run_disabled(); run_unpaired_and_early_outcome(); run_all_nine();
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
'''.replace("__BLOCK__", block))
(out / "provenance.txt").write_text(
    f"source={src}\nsource_sha256={h}\nblock_sha256={hashlib.sha256(block.encode()).hexdigest()}\n"
    f"source_block_lines={text[:start].count(chr(10))+1}-{text[:end].count(chr(10))+1}\n"
    "structural_boundary_checks=18\n"
    "flush_sites=retirement_timeout,no_image,after_gc_normal\n")
