#include "fec.h"
#include "frame_window.h"
#include "nack_deadline.h"
#include "shard_set.h"
#include "wivrn_packets.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

static int checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL:%d %s\n",__LINE__,#x); } } while(false)
namespace wivrn {
namespace application { struct config { bool shard_retransmit=true; }; inline config current{}; inline config &get_config(){return current;} }
namespace scenes { struct stream { std::vector<from_headset::nack> requests; void send_nack(const from_headset::nack &n){requests.push_back(n);} }; }
class shard_accumulator {
public:
    using shard_set=wivrn::shard_set;
    	using window_t = wivrn::frame_window<shard_set, 6, 3>;
    bool nxastc_codec=true;
    std::weak_ptr<scenes::stream> weak_scene;
    window_t window{shard_set{2}};
    std::vector<uint16_t> nack_scratch;
    uint64_t nack_requests=0, nack_shards=0;
    std::atomic<uint64_t> nack_shards_total=0;
    std::optional<XrTime> next_nack_deadline(XrTime);
    void poll_nacks(XrTime);
    void try_nack(XrTime);
    void report_nacks(XrTime) {}
};
/*
 * WiVRn VR streaming
 * Copyright (C) 2022-2023  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022-2023  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


std::optional<XrTime> shard_accumulator::next_nack_deadline(XrTime now)
{
	if (not nxastc_codec or not application::get_config().shard_retransmit)
		return {};
	auto scene = weak_scene.lock();
	if (not scene)
		return {};

	uint64_t newest = 0;
	bool any = false;
	window.for_each([&](shard_set & set) {
		if (set.empty())
			return;
		if (not any or set.frame_index() > newest)
		{
			newest = set.frame_index();
			any = true;
		}
	});
	if (not any)
		return {};

	std::optional<XrTime> earliest;
	window.for_each([&](shard_set & set) {
		auto due = nack_poll_deadline(
		        now,
		        set.last_shard,
		        set.nack_last,
		        set.nack_rounds,
		        true,
		        true,
		        not set.empty(),
		        set.complete(),
		        [&]() {
			        set.missing_shards(nack_scratch, set.frame_index() < newest);
			        return not nack_scratch.empty();
		        });
		if (due and (not earliest or *due < *earliest))
			earliest = *due;
	});
	return earliest;
}

void shard_accumulator::poll_nacks(XrTime now)
{
	if (nxastc_codec)
		try_nack(now);
}

void shard_accumulator::try_nack(XrTime now)
{
	// Read live rather than plumbed through: the headset sends its whole settings
	// block on every change and this is one bool on a path that already reads the
	// frame's worth of state around it.
	if (not application::get_config().shard_retransmit)
		return;

	// The newest frame anything has arrived for. Everything older than it has stopped
	// arriving in its own right — a hole in it is loss, not a shard still in the air —
	// and, more to the point, the shard past the highest one it holds is known to
	// exist, which is the one case where the frame length can be guessed at.
	uint64_t newest = 0;
	bool any = false;
	window.for_each([&](shard_set & set) {
		if (set.empty())
			return;
		if (not any or set.frame_index() > newest)
		{
			newest = set.frame_index();
			any = true;
		}
	});
	if (not any)
		return;

	window.for_each([&](shard_set & set) {
		// The clock first, because it is the cheap test and it is the one that
		// answers "no" for almost every frame: this runs on every shard arrival, and
		// the frame the shards belong to has by definition just been heard from.
		//
		// It is also the rate limit. One round in flight at a time: a request sent
		// less than the delay ago has not had the chance to be answered yet, and a
		// second copy of it would only spend bandwidth on the path that is already
		// dropping packets.
		const XrTime since = std::max(set.last_shard, set.nack_last);
		if (not nack_quiet_elapsed(now, since))
			return;
		if (set.nack_rounds >= nack_max_rounds or set.empty() or set.complete())
			return;

		set.missing_shards(nack_scratch, set.frame_index() < newest);
		if (nack_scratch.empty())
			return;

		auto scene = weak_scene.lock();
		if (not scene)
			return;

		const uint16_t first = nack_scratch.front();
		wivrn::from_headset::nack request{
		        .stream_index = set.feedback.stream_index,
		        .frame_idx = set.frame_index(),
		        .first_shard_idx = first,
		        .bitmap = {},
		};

		size_t asked = 0;
		for (uint16_t idx: nack_scratch)
		{
			const size_t bit = size_t(idx) - first;
			const size_t byte = bit / 8;
			// Past the window one request can name. A frame missing more than
			// 256 shards from its first hole is not one a retransmission round
			// is going to save.
			if (byte >= wivrn::from_headset::max_nack_bitmap_bytes)
				break;
			if (request.bitmap.size() <= byte)
				request.bitmap.resize(byte + 1, 0);
			request.bitmap[byte] |= uint8_t(1u << (bit % 8));
			++asked;
		}

		++set.nack_rounds;
		set.nack_last = now;
		++nack_requests;
		nack_shards += asked;
		nack_shards_total += asked;

		scene->send_nack(request);
	});

	report_nacks(now);
}

}
using namespace wivrn;
using data_shard=wivrn::to_headset::video_stream_data_shard;
using parity_shard=wivrn::to_headset::video_stream_parity_shard;
static std::shared_ptr<scenes::stream> live_scene(shard_accumulator &a){auto s=std::make_shared<scenes::stream>();a.weak_scene=s;return s;}
static shard_set &slot(shard_accumulator &a,uint64_t f){auto p=a.window.slot(f,[](shard_set&){}); CHECK(p!=nullptr); return *p;}
static void add(shard_accumulator&a,uint64_t f,uint16_t i,XrTime t,bool end=false){data_shard d{};d.stream_item_idx=2;d.frame_idx=f;d.shard_idx=i;if(end)d.timing_info=data_shard::timing_info_t{1,2,3,4};slot(a,f).insert(std::move(d),t);}
static std::string bits(const from_headset::nack&n){std::string s;for(size_t b=0;b<n.bitmap.size();++b)for(int j=0;j<8;++j)if(n.bitmap[b]&(1u<<j)){if(!s.empty())s+=';';s+=std::to_string(n.first_shard_idx+b*8+j);}return s;}
static void row(const char*case_name,int64_t now,const scenes::stream&s,size_t from,uint8_t rounds){if(from>=s.requests.size())std::printf("%s,%lld,0,-1,-1,,%u\n",case_name,(long long)now,unsigned(rounds));else for(size_t i=from;i<s.requests.size();++i){auto&n=s.requests[i];std::printf("%s,%lld,1,%llu,%u,%s,%u\n",case_name,(long long)now,(unsigned long long)n.frame_idx,unsigned(n.first_shard_idx),bits(n).c_str(),unsigned(rounds));}}
static void test_deadline_boundary_retry(){
 shard_accumulator a;auto s=live_scene(a);add(a,10,0,1000);add(a,10,1,1001);add(a,10,3,1002,true);add(a,11,0,1003);
 const int64_t due=1002+nack_quiet_period_ns;
 auto next=a.next_nack_deadline(due-1);CHECK(next&&*next==due);a.poll_nacks(due-1);CHECK(s->requests.empty());row("older_interior_before_due",due-1,*s,0,0);
 a.poll_nacks(due);CHECK(s->requests.size()==1&&s->requests[0].frame_idx==10&&s->requests[0].stream_index==2&&s->requests[0].first_shard_idx==2&&bits(s->requests[0])=="2");
 CHECK(a.nack_requests==1&&a.nack_shards==1&&a.nack_shards_total.load()==1&&slot(a,10).nack_rounds==1);row("older_interior_at_due",due,*s,0,slot(a,10).nack_rounds);
 const auto round2=due+nack_quiet_period_ns;CHECK(a.next_nack_deadline(round2-1)==round2);a.poll_nacks(round2-1);CHECK(s->requests.size()==1);row("older_interior_round2_before_due",round2-1,*s,1,slot(a,10).nack_rounds);
 a.poll_nacks(round2);CHECK(s->requests.size()==2&&slot(a,10).nack_rounds==2);CHECK(a.nack_requests==2&&a.nack_shards==2&&a.nack_shards_total.load()==2);row("older_interior_round2_at_due",round2,*s,1,slot(a,10).nack_rounds);
 a.poll_nacks(due+3*nack_quiet_period_ns);CHECK(s->requests.size()==2&&not a.next_nack_deadline(due+3*nack_quiet_period_ns));row("older_interior_after_round_limit",due+3*nack_quiet_period_ns,*s,2,slot(a,10).nack_rounds);
}
static void test_newest_interior(){
 shard_accumulator a;auto s=live_scene(a);add(a,15,0,1500);add(a,15,1,1501);add(a,15,3,1502,true);const auto due=1502+nack_quiet_period_ns;
 CHECK(a.next_nack_deadline(due)==due);a.poll_nacks(due);CHECK(s->requests.size()==1&&s->requests[0].frame_idx==15&&s->requests[0].stream_index==2&&bits(s->requests[0])=="2");row("newest_confirmed_interior",due,*s,0,slot(a,15).nack_rounds);
}
static void test_newest_tail_and_older_tail(){
 shard_accumulator newest;auto sn=live_scene(newest);add(newest,20,0,2000);add(newest,20,1,2001);
 const auto due=2001+nack_quiet_period_ns;CHECK(not newest.next_nack_deadline(due));newest.poll_nacks(due);CHECK(sn->requests.empty());row("newest_unknown_tail",due,*sn,0,0);
 shard_accumulator older;auto so=live_scene(older);add(older,30,0,3000);add(older,30,1,3001);add(older,31,0,3002);
 const auto odue=3002+nack_quiet_period_ns;older.poll_nacks(odue);CHECK(so->requests.size()==1&&so->requests[0].stream_index==2&&so->requests[0].frame_idx==30&&so->requests[0].first_shard_idx==2&&bits(so->requests[0])=="2");row("older_inferred_tail",odue,*so,0,slot(older,30).nack_rounds);
}
static void test_gates(){
 shard_accumulator off;auto s=live_scene(off);add(off,40,0,4000);add(off,40,2,4001,true);application::current.shard_retransmit=false;CHECK(not off.next_nack_deadline(4001+nack_quiet_period_ns));off.poll_nacks(4001+nack_quiet_period_ns);CHECK(s->requests.empty()&&slot(off,40).nack_rounds==0);application::current.shard_retransmit=true;row("retransmit_off",4001+nack_quiet_period_ns,*s,0,0);
 shard_accumulator non;non.nxastc_codec=false;auto ns=live_scene(non);add(non,50,0,5000);add(non,50,2,5001,true);auto due=5001+nack_quiet_period_ns;CHECK(not non.next_nack_deadline(due));non.poll_nacks(due);CHECK(ns->requests.empty()&&slot(non,50).nack_rounds==0);row("non_astc",due,*ns,0,0);
 shard_accumulator expired;add(expired,60,0,6000);add(expired,60,2,6001,true);auto dead=std::make_shared<scenes::stream>();expired.weak_scene=dead;dead.reset();auto edue=6001+nack_quiet_period_ns;CHECK(not expired.next_nack_deadline(edue));expired.poll_nacks(edue);CHECK(slot(expired,60).nack_rounds==0);row("expired_scene",edue,scenes::stream{},0,0);
}
static void test_real_fec_before_nack_then_reply(){
 shard_accumulator a;auto scene=live_scene(a);auto &set=slot(a,70);std::array<uint8_t,3> p0{1,2,3},p1{4,5,6},p2{7,8,9},p3{10,11,12};
 data_shard d0{};d0.stream_item_idx=2;d0.frame_idx=70;d0.shard_idx=0;d0.payload=p0;
 data_shard d1{};d1.stream_item_idx=2;d1.frame_idx=70;d1.shard_idx=1;d1.payload=p1;
 data_shard d2{};d2.stream_item_idx=2;d2.frame_idx=70;d2.shard_idx=2;d2.payload=p2;
 data_shard d3{};d3.stream_item_idx=2;d3.frame_idx=70;d3.shard_idx=3;d3.timing_info=data_shard::timing_info_t{1,2,3,4};d3.payload=p3;
 wivrn::fec::group_builder builder;builder.set_layout(2,1);builder.reset(2,70);builder.add(d0);builder.add(d1);auto parity=builder.take();CHECK(parity.has_value());
 set.insert(std::move(d0),7000);set.insert(std::move(d3),7001);set.parity.push_back(*parity);
 // Match push_shard ordering: real one-erasure reconstruction/insertion precedes try_nack.
 auto rebuilt=set.reconstruct(set.parity[0],7002);CHECK(rebuilt&&*rebuilt==1);
 CHECK(set.shards()[1]&&set.shards()[1]->payload.size()==p1.size()&&std::equal(set.shards()[1]->payload.begin(),set.shards()[1]->payload.end(),p1.begin()));
 set.parity.clear();std::vector<uint16_t> missing;set.missing_shards(missing,false);CHECK(missing.size()==1&&missing[0]==2);
 const auto due=7002+nack_quiet_period_ns;CHECK(a.next_nack_deadline(due)==due);a.poll_nacks(due);
 CHECK(scene->requests.size()==1&&scene->requests[0].stream_index==2&&scene->requests[0].frame_idx==70&&bits(scene->requests[0])=="2");CHECK(a.nack_requests==1&&a.nack_shards==1&&a.nack_shards_total.load()==1&&set.nack_rounds==1);row("fec_repair_then_nack_other_hole",due,*scene,0,set.nack_rounds);
 set.insert(std::move(d2),due+1);CHECK(set.complete());set.missing_shards(missing,false);CHECK(missing.empty());
 a.poll_nacks(due+2*nack_quiet_period_ns);CHECK(scene->requests.size()==1&&set.nack_rounds==1);row("nack_reply_completes_frame",due+2*nack_quiet_period_ns,*scene,1,set.nack_rounds);
}
int main(){
 std::puts("case,virtual_now,request_count,frame,first_shard,requested_indices,rounds");
 test_deadline_boundary_retry();test_newest_interior();test_newest_tail_and_older_tail();test_gates();test_real_fec_before_nack_then_reply();
 std::printf("checks=%d failures=%d\n",checks,failures);return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
