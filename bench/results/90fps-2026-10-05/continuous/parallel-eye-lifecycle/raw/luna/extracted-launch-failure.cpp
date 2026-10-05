#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>
using namespace std::chrono_literals;

static std::atomic<int> checks{0}, failures{0}, warning_count{0};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL:%d: %s\n", __LINE__, #x); } } while (0)
#define U_LOG_W(...) (++warning_count)
constexpr int quad_stream_idx = 3;
struct ViewInfo { bool alpha = true; int foveation = 1; std::optional<int> quad; };
struct Session {};
struct Image { std::atomic<bool> busy{true}; uint64_t frame_index = 7; ViewInfo view_info{}; std::optional<int> quad_info; };
struct video_encoder {
    explicit video_encoder(int i): stream_idx(i) {}
    virtual ~video_encoder() = default;
    int stream_idx;
    virtual void encode(Session &, const ViewInfo &, uint64_t) = 0;
};
struct video_encoder_astc : video_encoder { using video_encoder::video_encoder; };
struct Shared {
    std::mutex m; std::condition_variable cv;
    bool right_started=false, release_right=false, left_done=false, expect_parallel=false;
    bool block_right=false, throw_left=false, throw_right=false;
    int left_calls=0, right_calls=0, aux_calls=0, quad_calls=0, replacement_calls=0;
    int active=0, max_active=0;
    bool overlap=false;
};
static void outcome(const char *name, const Shared &s, bool busy, bool snapshot_alive=0, bool snapshot_expired=0, int overlap_override=-1) {
    const int overlap = overlap_override < 0 ? int(s.overlap) : overlap_override;
    std::printf("outcome,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", name, overlap, s.max_active,
                s.left_calls, s.right_calls, s.aux_calls, int(busy), snapshot_alive, snapshot_expired, s.quad_calls);
}
struct FakeAstc : video_encoder_astc {
    FakeAstc(int i, Shared &s, bool replacement=false): video_encoder_astc(i), s(s), replacement(replacement) {}
    Shared &s; bool replacement;
    void encode(Session &, const ViewInfo &, uint64_t) override {
        bool should_throw=false;
        {
            std::unique_lock lock(s.m);
            ++s.active; s.max_active=std::max(s.max_active,s.active);
            if (replacement) ++s.replacement_calls;
            else if (stream_idx==0) ++s.left_calls;
            else ++s.right_calls;
            if (stream_idx==1 && !replacement) { s.right_started=true; s.cv.notify_all(); }
            if (stream_idx==0 && s.expect_parallel) {
                s.cv.wait_for(lock, 2s, [&]{ return s.right_started; });
                s.overlap = s.right_started && s.active >= 2;
            }
            if (stream_idx==1 && s.block_right && !replacement)
                s.cv.wait(lock, [&]{ return s.release_right; });
            if (stream_idx==0) should_throw=s.throw_left;
            if (stream_idx==1) should_throw=s.throw_right;
            --s.active;
            if (stream_idx==0) { s.left_done=true; s.cv.notify_all(); }
        }
        if (should_throw) throw std::runtime_error("fake encoder error");
    }
};
struct FakeHardware : video_encoder {
    FakeHardware(int i, Shared &s): video_encoder(i),s(s) {} Shared &s;
    void encode(Session &, const ViewInfo &, uint64_t) override { std::lock_guard l(s.m); ++s.left_calls; ++s.active; s.max_active=std::max(s.max_active,s.active); --s.active; }
};
struct FakeAux : video_encoder {
    explicit FakeAux(Shared &s):video_encoder(2),s(s) {} Shared &s;
    void encode(Session &, const ViewInfo &, uint64_t) override { std::lock_guard l(s.m); ++s.aux_calls; }
};
struct FakeQuad : video_encoder {
    explicit FakeQuad(Shared &s):video_encoder(3),s(s) {} Shared &s;
    void encode(Session &, const ViewInfo &view, uint64_t) override { std::lock_guard l(s.m); ++s.quad_calls; assert(view.quad.has_value() && view.foveation==0); }
};
#ifdef INJECT_LAUNCH_FAILURE
static std::atomic<bool> fail_next_launch{false};
template<class F> std::future<void> injected_async(std::launch policy, F &&f) {
    if (fail_next_launch.exchange(false)) throw std::runtime_error("injected launch failure");
    return std::async(policy, std::forward<F>(f));
}
#endif
struct Harness {
    Shared &s; Image image; Session session; std::mutex enc_mutex;
    std::array<std::shared_ptr<video_encoder>,4> current{};
    Harness(Shared &s):s(s) {}
    auto get_encoders() { std::lock_guard l(enc_mutex); return current; }
    void replace_right(std::shared_ptr<video_encoder> p) { std::lock_guard l(enc_mutex); current[1]=std::move(p); }
    void work() {
        const char * parallel_option = std::getenv("WIVRN_ASTC_PARALLEL_EYES");
        const bool parallel_eyes = parallel_option && std::string_view(parallel_option) == "1";
		// One copy for the whole frame: an encoder replaced from the present
		// thread half way through this loop must not change which object the
		// rest of it talks to, and the one it replaced stays alive as long as
		// this copy does.
		auto encoders = get_encoders();
		auto encode_one = [&](const std::shared_ptr<video_encoder> & encoder) {
			if (not encoder)
				return;

			// Per encoder rather than around the loop: one stream's driver
			// giving up must not cost the other eyes their frame as well.
			try
			{
				if (encoder->stream_idx == quad_stream_idx)
				{
					if (not image.quad_info)
						return;
					// Same frame index and same view info as the eyes,
					// with the quad's placement attached and the eye
					// foveation dropped: nothing on this stream is
					// foveated and the headset never reads it there.
					auto view_info = image.view_info;
					view_info.foveation = {};
					view_info.quad = image.quad_info;
					encoder->encode(session, view_info, image.frame_index);
				}
				else if (encoder->stream_idx < 2 or image.view_info.alpha)
					encoder->encode(session, image.view_info, image.frame_index);
			}
			catch (std::exception & e)
			{
				// The watchdog has the same news, and it is the one that
				// decides what to do about it.
				U_LOG_W("encode error on stream %d: %s", encoder->stream_idx, e.what());
			}
		};

		// Only independent native ASTC eyes opt in. Keep auxiliary and hardware
		// encoders serial until their shared state has been checked separately.
		const bool independent_astc = parallel_eyes &&
		        dynamic_cast<video_encoder_astc *>(encoders[0].get()) &&
		        dynamic_cast<video_encoder_astc *>(encoders[1].get()) &&
		        std::ranges::none_of(std::span(encoders).subspan(2), [](const auto & e) { return bool(e); });
		std::future<void> right_eye;
		if (independent_astc)
		{
			try
			{
				right_eye = injected_async(std::launch::async, [&, encoder = encoders[1]] { encode_one(encoder); });
			}
			catch (const std::exception & e)
			{
				U_LOG_W("ASTC eye worker unavailable; encoding serially: %s", e.what());
			}
		}
		for (size_t i = 0; i < encoders.size(); ++i)
			if (i != 1 || !right_eye.valid())
				encode_one(encoders[i]);
		// Drain before reusing the image or taking the next frame. Each eye keeps
		// its own codec state, and the snapshot survives hot encoder replacement.
		if (right_eye.valid())
			right_eye.get();
		image.busy = false;
    }
};
static bool wait_for(std::condition_variable &cv, std::mutex &m, const auto &pred) {
    std::unique_lock lock(m); return cv.wait_for(lock, 2s, pred);
}
static void serial_case(const char *option, int kind) {
    Shared s; Harness h(s); auto left=std::make_shared<FakeAstc>(0,s); auto right=std::make_shared<FakeAstc>(1,s);
    if (kind==1) h.current[0]=std::make_shared<FakeHardware>(0,s); else h.current[0]=left;
    h.current[1]=right; if (kind==2) h.current[2]=std::make_shared<FakeAux>(s);
    if (kind==3) h.current[1].reset();
    if (kind==4) { h.current[3]=std::make_shared<FakeQuad>(s); h.image.quad_info=1; }
    if (option) setenv("WIVRN_ASTC_PARALLEL_EYES",option,1); else unsetenv("WIVRN_ASTC_PARALLEL_EYES");
    h.work(); CHECK(!h.image.busy.load());
    CHECK(s.max_active==1);
    CHECK(s.left_calls==1);
    CHECK(s.right_calls==(kind==3 ? 0 : 1));
    CHECK(s.aux_calls==(kind==2 ? 1 : 0)); CHECK(s.quad_calls==(kind==4 ? 1 : 0));
    const char *names[]={"hardware_fallback", "auxiliary_fallback", "missing_eye_fallback", "quad_fallback"};
    if (kind==0) outcome(option ? (std::string_view(option)=="true" ? "nonexact_flag_fallback" : "option_off_fallback") : "option_absent_fallback",s,h.image.busy.load());
    else outcome(names[kind-1],s,h.image.busy.load());
}
static void parallel_lifetime_and_snapshot() {
    Shared s; s.expect_parallel=true; s.block_right=true; Harness h(s);
    auto left=std::make_shared<FakeAstc>(0,s), old_right=std::make_shared<FakeAstc>(1,s);
    auto replacement=std::make_shared<FakeAstc>(1,s,true); h.current[0]=left; h.current[1]=old_right;
    setenv("WIVRN_ASTC_PARALLEL_EYES","1",1);
    std::thread worker([&]{h.work();});
    bool entered=wait_for(s.cv,s.m,[&]{return s.right_started && s.left_done;});
    CHECK(entered); CHECK(h.image.busy.load()); CHECK(s.overlap);
    std::weak_ptr<FakeAstc> old_weak=old_right;
    h.replace_right(replacement); old_right.reset(); const bool alive_blocked=!old_weak.expired(); CHECK(alive_blocked);
    { std::lock_guard lock(s.m); s.release_right=true; s.cv.notify_all(); }
    worker.join();
    CHECK(!h.image.busy.load()); CHECK(s.left_calls==1); CHECK(s.right_calls==1);
    CHECK(s.replacement_calls==0); CHECK(s.max_active>=2); CHECK(old_weak.expired());
    outcome("parallel_two_astc_snapshot_release",s,h.image.busy.load(),alive_blocked,old_weak.expired());
}
static void per_eye_exception_releases() {
    for (int failing_eye=0; failing_eye<2; ++failing_eye) {
        const int warnings_before=warning_count.load(); Shared s; s.throw_left=(failing_eye==0); s.throw_right=(failing_eye==1); Harness h(s);
        h.current[0]=std::make_shared<FakeAstc>(0,s); h.current[1]=std::make_shared<FakeAstc>(1,s);
        setenv("WIVRN_ASTC_PARALLEL_EYES","1",1); h.work();
        CHECK(s.left_calls==1); CHECK(s.right_calls==1); CHECK(warning_count.load()==warnings_before+1); CHECK(!h.image.busy.load());
        outcome(failing_eye==0 ? "left_exception" : "right_exception",s,h.image.busy.load());
    }
}
#ifdef INJECT_LAUNCH_FAILURE
static void launch_failure_falls_back_once() {
    Shared s; Harness h(s); h.current[0]=std::make_shared<FakeAstc>(0,s); h.current[1]=std::make_shared<FakeAstc>(1,s);
    setenv("WIVRN_ASTC_PARALLEL_EYES","1",1); fail_next_launch=true; h.work();
    CHECK(s.left_calls==1); CHECK(s.right_calls==1); CHECK(s.max_active==1); CHECK(!h.image.busy.load());
    outcome("injected_launch_failure_fallback",s,h.image.busy.load());
}
#endif
int main() {
    std::puts("outcome,scenario,overlap,max_active,left_calls,right_calls,aux_calls,image_busy_after,snapshot_alive_while_blocked,snapshot_expired_after_join,quad_calls");
    serial_case(nullptr,0); serial_case("0",0); serial_case("true",0); serial_case("1",1); serial_case("1",2); serial_case("1",3); serial_case("1",4);
    parallel_lifetime_and_snapshot(); per_eye_exception_releases();
#ifdef INJECT_LAUNCH_FAILURE
    launch_failure_falls_back_once();
#endif
    std::printf("%d checks, %d failures\n",checks.load(),failures.load()); return failures.load()?1:0;
}
