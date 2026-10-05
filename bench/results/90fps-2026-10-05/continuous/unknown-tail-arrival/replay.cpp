// Include existing production-class test fixtures: make_frame(), send_frame(), etc.
#define main nack_test_suite_main
#include "tests/nack_test.cpp"
#undef main

#include "nack_deadline.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace
{
using clock_t = int64_t;
constexpr clock_t refresh_90hz_ns = 11'111'111;
constexpr clock_t stale_after_ns = 2 * refresh_90hz_ns;
constexpr size_t response_cap = 64;

void cache_one(shard_history &history, const data_shard &shard)
{
	std::vector<uint8_t> blob;
	fec::encode_blob(shard, blob);
	history.push(shard.frame_idx, shard.shard_idx, blob, true);
}

std::vector<data_shard> answer(shard_history &history,
                               const from_headset::nack &request,
                               bool end_assist = true)
{
	std::vector<data_shard> result;
	const uint32_t asked = uint32_t(std::popcount(request.bitmap[0]));
	history.note_nacked(request.frame_idx, asked);
	std::vector<shard_history::hit> hits;
	const size_t found = history.collect(request.frame_idx, request.first_shard_idx,
	                                     request.bitmap, response_cap, hits);
	for (const auto &hit: hits)
		result.push_back(fec::decode_blob(request.stream_index, request.frame_idx, hit.shard_idx, hit.blob));
	if (end_assist and found != 0 and result.size() < response_cap)
		if (auto end = history.collect_frame_end_candidate(request.frame_idx, hits, response_cap - result.size()))
		{
			auto shard = fec::decode_blob(request.stream_index, request.frame_idx, end->shard_idx, end->blob);
			if (shard.timing_info)
				result.push_back(std::move(shard));
		}
	return result;
}

from_headset::nack one_index_request(uint8_t stream, uint64_t frame_idx, uint16_t index)
{
	const std::array<uint16_t, 1> one{index};
	return make_request(stream, frame_idx, one);
}

bool probe_due(const shard_set &set, clock_t now, clock_t first_received, bool is_newest = true)
{
	std::vector<uint16_t> known_holes;
	set.missing_shards(known_holes, false);
	return is_newest and not set.complete() and known_holes.empty() and set.nack_rounds < nack_max_rounds and
	       first_received > 0 and now >= first_received and uint64_t(now - first_received) >= uint64_t(stale_after_ns) and
	       nack_quiet_elapsed(now, std::max(set.last_shard, set.nack_last));
}

bool insert_all_but_terminal(shard_set &set, const frame &f, clock_t time)
{
	for (size_t i = 0; i + 1 < f.shards.size(); ++i)
		set.insert(data_shard(f.shards[i]), time + int64_t(i));
	return not set.complete();
}

struct row
{
	const char *name;
	bool baseline_complete;
	bool candidate_complete;
	unsigned requests;
	unsigned replies;
	const char *detail;
};

std::vector<row> rows;
unsigned replay_failures = 0;
unsigned replay_checks = 0;

void check(bool condition, const char *label)
{
	++replay_checks;
	if (!condition)
	{
		++replay_failures;
		std::fprintf(stderr, "FAIL: %s\n", label);
	}
}

void run_completed_terminal()
{
	frame f = make_frame(5, 100, 4, 1);
	shard_set baseline(0), candidate(0);
	baseline.reset(100); candidate.reset(100);
	insert_all_but_terminal(baseline, f, 1);
	insert_all_but_terminal(candidate, f, 1);
	shard_history h; h.set_enabled(true); send_frame(h, f);
	const bool due = probe_due(candidate, stale_after_ns + 3'000'000, 1);
	check(due, "completed terminal probe due at stale gate");
	candidate.nack_rounds++;
	candidate.nack_last = stale_after_ns + 3'000'000;
	auto replies = answer(h, one_index_request(0, 100, uint16_t(candidate.shards().size())));
	for (auto &s: replies) candidate.insert(std::move(s), stale_after_ns + 4'000'000);
	check(replies.size() == 1 && replies[0].timing_info.has_value(), "terminal reply contains real timing marker");
	check(replies.size() == 1 && same_shard(replies[0], f.shards.back()), "terminal payload/metadata match source shard");
	check(candidate.complete() && !baseline.complete(), "terminal probe closes frame; baseline does not");
	rows.push_back({"sender_complete_terminal_lost", baseline.complete(), candidate.complete(), 1,
	                unsigned(replies.size()), "one request; exact cached terminal; no new frame required"});
}

void run_uncached_then_arrives()
{
	frame f = make_frame(5, 101, 4, 1);
	shard_set candidate(0); candidate.reset(101); insert_all_but_terminal(candidate, f, 1);
	shard_history h; h.set_enabled(true);
	for (size_t i = 0; i + 1 < f.shards.size(); ++i) cache_one(h, f.shards[i]);
	const bool due = probe_due(candidate, stale_after_ns + 3'000'000, 1);
	check(due, "uncached terminal probe due");
	candidate.nack_rounds++;
	candidate.nack_last = stale_after_ns + 3'000'000;
	auto miss = answer(h, one_index_request(0, 101, 4));
	check(miss.empty(), "uncached terminal request misses");
	cache_one(h, f.shards.back()); h.end_frame(101, uint32_t(f.shards.size()));
	const bool retry_due = probe_due(candidate, stale_after_ns + 6'000'000, 1);
	check(retry_due && candidate.nack_rounds == 1, "one remaining retry remains after miss");
	candidate.nack_rounds++;
	auto hit = answer(h, one_index_request(0, 101, 4));
	for (auto &s: hit) candidate.insert(std::move(s), stale_after_ns + 7'000'000);
	check(candidate.complete(), "second round retrieves terminal after sender completion");
	check(hit.size() == 1 && same_shard(hit[0], f.shards.back()), "retry returns exact terminal shard");
	rows.push_back({"uncached_then_cached", false, candidate.complete(), 2,
	                unsigned(hit.size()), "first miss consumes round; second gets terminal"});
}

void run_cached_before_end_frame()
{
	frame f = make_frame(5, 102, 4, 1);
	shard_set candidate(0); candidate.reset(102); insert_all_but_terminal(candidate, f, 1);
	shard_history h; h.set_enabled(true);
	for (size_t i = 0; i < f.shards.size(); ++i) cache_one(h, f.shards[i]);
	// Simulates terminal blob stored before sender records end_frame count.
	check(!h.frame_cost(102).has_value(), "sender count absent before end_frame");
	auto replies = answer(h, one_index_request(0, 102, 4));
	for (auto &s: replies) candidate.insert(std::move(s), stale_after_ns + 4'000'000);
	check(candidate.complete() && replies.size() == 1, "ordinary requested terminal works before end_frame");
	check(replies.size() == 1 && same_shard(replies[0], f.shards.back()), "pre-end_frame hit is exact terminal");
	rows.push_back({"cached_terminal_before_end_frame", false, candidate.complete(), 1,
	                unsigned(replies.size()), "ordinary hit succeeds; end assist unavailable"});
}

void run_delayed_parity()
{
	frame f = make_frame(4, 103, 4, 1);
	const size_t lost[] = {3};
	shard_set candidate = receive(f, lost, false);
	check(!candidate.complete(), "terminal missing before parity");
	// Late parity repairs using actual shard_set::reconstruct path, before poll.
	candidate.parity.push_back(f.parity.front());
	auto repaired = candidate.reconstruct(candidate.parity.back(), stale_after_ns + 3'000'000);
	check(repaired && *repaired == 3 && candidate.complete(), "delayed parity repairs terminal before probe");
	check(repaired && same_shard(*candidate.shards()[3], f.shards[3]), "parity reconstruction preserves terminal payload/metadata");
	check(!probe_due(candidate, stale_after_ns + 4'000'000, 1), "complete parity repair suppresses probe");
	rows.push_back({"delayed_parity_before_probe", false, candidate.complete(), 0, 0,
	                "late parity reconstructs exact terminal; no request"});
}

void run_interior_priority()
{
	frame f = make_frame(8, 104, 4, 1);
	const size_t lost[] = {2, 7};
	shard_set set = receive(f, lost, false);
	std::vector<uint16_t> holes;
	set.missing_shards(holes, false);
	check(holes.size() == 1 && holes[0] == 2, "confirmed interior hole remains first work");
	check(read_request(make_request(0, 104, holes)) == holes, "confirmed-hole bitmap remains ordinary request");
	check(!probe_due(set, stale_after_ns + 3'000'000, 1), "tail probe does not displace confirmed hole");
	rows.push_back({"interior_hole_priority", false, false, 1, 0,
	                "ordinary confirmed hole blocks speculative tail probe"});
}

void run_frame_advance()
{
	frame f = make_frame(5, 105, 4, 1);
	shard_set older(0); older.reset(105); insert_all_but_terminal(older, f, 1);
	shard_set newer(0); newer.reset(106);
	frame newer_frame = make_frame(1, 106, 1, 1);
	newer.insert(data_shard(newer_frame.shards[0]), stale_after_ns + 4'000'000);
	std::vector<uint16_t> old_missing;
	older.missing_shards(old_missing, older.frame_index() < newer.frame_index());
	check(old_missing.size() == 1 && old_missing[0] == 4, "frame advance enables existing next-index NACK");
	check(read_request(make_request(0, 105, old_missing)) == old_missing, "frame-over index uses existing NACK bitmap");
	check(!probe_due(older, stale_after_ns + 5'000'000, 1, false), "advance path needs no separate stale probe");
	rows.push_back({"newer_frame_arrives", false, false, 1, 0,
	                "existing frame_over rule requests next index"});
}


void run_parity_before_reply()
{
	frame f = make_frame(4, 108, 4, 1);
	shard_set candidate(0); candidate.reset(108); insert_all_but_terminal(candidate, f, 1);
	shard_history h; h.set_enabled(true); send_frame(h, f);
	check(probe_due(candidate, stale_after_ns + 3'000'000, 1), "probe due before parity/reply race");
	auto replies = answer(h, one_index_request(0, 108, 3)); // NACK sent; hold response
	check(replies.size() == 1 && same_shard(replies[0], f.shards.back()), "server cached exact terminal reply");
	candidate.parity.push_back(f.parity.front());
	auto repaired = candidate.reconstruct(candidate.parity.back(), stale_after_ns + 4'000'000);
	check(repaired && *repaired == 3 && candidate.complete(), "parity completes receiver before NACK reply arrives");
	const size_t count_before = size_t(std::ranges::count_if(candidate.shards(), [](const auto &entry) { return entry.has_value(); }));
	const XrTime last_before = candidate.last_shard;
	const auto duplicate = candidate.insert(std::move(replies[0]), stale_after_ns + 5'000'000);
	check(!duplicate && size_t(std::ranges::count_if(candidate.shards(), [](const auto &entry) { return entry.has_value(); })) == count_before &&
	      candidate.last_shard == last_before,
	      "late repair is duplicate; no count or arrival-time mutation");
	check(candidate.complete() && same_shard(*candidate.shards()[3], f.shards.back()),
	      "duplicate leaves payload and timing metadata intact");
	rows.push_back({"parity_before_nack_reply", false, candidate.complete(), 1,
	                unsigned(replies.size()), "one requested reply may be redundant; actual duplicate insert is ignored"});
}

void run_round_cap()
{
	frame f = make_frame(5, 109, 4, 1);
	shard_set set(0); set.reset(109); insert_all_but_terminal(set, f, 1);
	set.nack_rounds = nack_max_rounds;
	check(!probe_due(set, stale_after_ns + 3'000'000, 1), "probe stops at existing two-round ceiling");
	rows.push_back({"two_round_cap", false, false, 0, 0, "no third probe"});
}

void run_stream_metadata()
{
	frame f = make_frame(3, 107, 2, 1);
	for (auto &shard: f.shards) shard.stream_item_idx = 1;
	shard_set set(1); set.reset(107);
	for (size_t i = 0; i + 1 < f.shards.size(); ++i) set.insert(data_shard(f.shards[i]), 1 + int64_t(i));
	const auto &view = *set.shards()[0]->view_info;
	check(set.feedback.stream_index == 1 && set.shards()[0]->stream_item_idx == 1 &&
	      view.display_time == 42 && view.foveation.size() == 2 &&
	      view.pose[0].position.x == 0.03f && view.pose[1].position.x == -0.03f,
	      "stream 1 owns first-shard stereo view with expected pose/display values");
	auto req = one_index_request(set.feedback.stream_index, 107, 2);
	check(req.stream_index == 1 && req.frame_idx == 107, "probe NACK names owning stream and frame");
	rows.push_back({"stereo_stream_metadata", false, false, 0, 0,
	                "existing set retains stream index and first-shard view metadata"});
}
} // namespace

int main()
{
	run_completed_terminal();
	run_uncached_then_arrives();
	run_cached_before_end_frame();
	run_delayed_parity();
	run_parity_before_reply();
	run_round_cap();
	run_interior_priority();
	run_frame_advance();
	run_stream_metadata();
	std::puts("case,baseline_complete,candidate_complete,requests,replies,notes");
	for (const row &r: rows)
		std::printf("%s,%d,%d,%u,%u,\"%s\"\n", r.name, r.baseline_complete, r.candidate_complete,
		            r.requests, r.replies, r.detail);
	std::fprintf(stderr, "checks=%u failures=%u\n", replay_checks, replay_failures);
	return replay_failures ? 1 : 0;
}
