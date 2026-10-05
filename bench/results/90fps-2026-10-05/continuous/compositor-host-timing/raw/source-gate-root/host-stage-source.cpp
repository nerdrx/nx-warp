#include <array>
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
    	const bool host_timing = session.dump_timings_enabled();
	const uint64_t host_frame_id = frame.rendering.id;
	static constexpr std::array host_stage_names{
	        "compositor_retirement_poll", "compositor_acquire", "compositor_record",
	        "compositor_queue_lock", "compositor_submit", "compositor_encoder_present",
	        "compositor_timeline_wait", "compositor_query_wait", "compositor_gc"};
	std::array<int64_t, 2 * host_stage_names.size()> host_times{};
	auto host_stamp = [&](size_t index) {
		if (host_timing)
			host_times[index] = os_monotonic_get_ns();
	};
	auto host_dump = [&](uint64_t frame_id, const char * outcome) {
		if (host_timing)
			for (size_t stage = 0; stage < host_stage_names.size(); ++stage)
				if (host_times[2 * stage] and host_times[2 * stage + 1])
					session.dump_time(host_stage_names[stage], frame_id, host_times[2 * stage + 1], uint8_t(-1),
					                  fake_format(host_times[2 * stage], outcome).c_str());
	};
    host_stamp(0); host_stamp(1); host_dump(99, "submitted");
    (void)host_frame_id;
    CHECK(clock_calls==0 && format_calls==0 && session.rows.empty());
}
void run_unpaired_and_early_outcome() {
    FakeSession session; session.enabled=true; FakeFrame frame;
    	const bool host_timing = session.dump_timings_enabled();
	const uint64_t host_frame_id = frame.rendering.id;
	static constexpr std::array host_stage_names{
	        "compositor_retirement_poll", "compositor_acquire", "compositor_record",
	        "compositor_queue_lock", "compositor_submit", "compositor_encoder_present",
	        "compositor_timeline_wait", "compositor_query_wait", "compositor_gc"};
	std::array<int64_t, 2 * host_stage_names.size()> host_times{};
	auto host_stamp = [&](size_t index) {
		if (host_timing)
			host_times[index] = os_monotonic_get_ns();
	};
	auto host_dump = [&](uint64_t frame_id, const char * outcome) {
		if (host_timing)
			for (size_t stage = 0; stage < host_stage_names.size(); ++stage)
				if (host_times[2 * stage] and host_times[2 * stage + 1])
					session.dump_time(host_stage_names[stage], frame_id, host_times[2 * stage + 1], uint8_t(-1),
					                  fake_format(host_times[2 * stage], outcome).c_str());
	};
    host_stamp(2); host_stamp(3); host_stamp(4); // incomplete record stage
    host_dump(host_frame_id, "no_image");
    CHECK(clock_calls==3 && format_calls==1 && session.rows.size()==1);
    CHECK(session.rows[0].name=="compositor_acquire" && session.rows[0].frame==13);
    CHECK(session.rows[0].stream==255 && session.rows[0].time>extra_start(session.rows[0].extra));
    CHECK(session.rows[0].extra.find(",no_image")!=std::string::npos);
}
void run_all_nine() {
    FakeSession session; session.enabled=true; FakeFrame frame;
    	const bool host_timing = session.dump_timings_enabled();
	const uint64_t host_frame_id = frame.rendering.id;
	static constexpr std::array host_stage_names{
	        "compositor_retirement_poll", "compositor_acquire", "compositor_record",
	        "compositor_queue_lock", "compositor_submit", "compositor_encoder_present",
	        "compositor_timeline_wait", "compositor_query_wait", "compositor_gc"};
	std::array<int64_t, 2 * host_stage_names.size()> host_times{};
	auto host_stamp = [&](size_t index) {
		if (host_timing)
			host_times[index] = os_monotonic_get_ns();
	};
	auto host_dump = [&](uint64_t frame_id, const char * outcome) {
		if (host_timing)
			for (size_t stage = 0; stage < host_stage_names.size(); ++stage)
				if (host_times[2 * stage] and host_times[2 * stage + 1])
					session.dump_time(host_stage_names[stage], frame_id, host_times[2 * stage + 1], uint8_t(-1),
					                  fake_format(host_times[2 * stage], outcome).c_str());
	};
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
