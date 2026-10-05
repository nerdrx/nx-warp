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

#include "frame_window.h"
#include "nack_deadline.h"
#include "shard_set.h"
#include "wivrn_packets.h"
#include <memory>
#include <cstdio>
#include <vector>
#include <atomic>
#include <spdlog/spdlog.h>
namespace wivrn {
namespace application { struct config { bool shard_retransmit=true; }; inline config current{}; inline config& get_config(){return current;} }
namespace scenes { struct stream { std::vector<from_headset::nack> requests; void send_nack(const from_headset::nack&n){requests.push_back(n);} XrDuration display_period_ns()const{return 11'111'111;} }; }
struct fake_instance { XrTime value=1; XrTime now()const{return value;} };
struct fake_decoder {
 std::vector<uint64_t> completed;
 size_t bytes=0;
 void push_data(std::span<const std::span<const uint8_t>> payload,uint64_t,bool){for(auto p:payload)bytes+=p.size();}
 void frame_completed(const from_headset::feedback&fb,const to_headset::video_stream_data_shard::view_info_t&){completed.push_back(fb.frame_index);}
};
class shard_accumulator {
public:
 using shard_set=wivrn::shard_set;
 using data_shard=to_headset::video_stream_data_shard;
 	using window_t = wivrn::frame_window<shard_set, 6, 3>;
 window_t window{shard_set{2}};
 fake_instance instance;
 std::shared_ptr<fake_decoder> decoder_=std::make_shared<fake_decoder>();
 std::weak_ptr<scenes::stream> weak_scene;
 bool astc_deadline_enabled=false;
 bool nxastc_codec=true;
 std::vector<uint16_t> nack_scratch;
 uint64_t nack_requests=0,nack_shards=0;
 std::atomic<uint64_t> nack_shards_total=0;
 void report_nacks(XrTime){}
 void poll_nacks(XrTime);
 void try_nack(XrTime);
 std::vector<uint64_t> feedback_frames;
 void send_feedback(from_headset::feedback&fb){feedback_frames.push_back(fb.frame_index);}
 void pump(XrTime);
 window_t::step try_submit_front(shard_set&);
};
static void debug_why_not_sent(const shard_accumulator::shard_set&){}
void shard_accumulator::pump(XrTime now)
{
	XrDuration period = 0;
	if (astc_deadline_enabled)
		if (const auto scene = weak_scene.lock())
			period = scene->display_period_ns();
	window.drain(
	        [this](shard_set & set) { return try_submit_front(set); },
	        [this](shard_set & set) {
		        debug_why_not_sent(set);
		        send_feedback(set.feedback);
	        },
	        [this, now, period](const shard_set & set) {
		        return frame_deadline_expired(now, set.feedback.received_first_packet, period,
		                                      astc_deadline_enabled, window.has_newer_complete_than_front());
	        });
}

shard_accumulator::window_t::step shard_accumulator::try_submit_front(shard_set & current)
{
	using step = window_t::step;
	const auto & data_shards = current.shards();

	// Everything before `submitted` is already in the decoder's input buffer, and
	// the decoder appends what it is given: the run to hand over starts there and
	// stops at the first hole.
	uint16_t first = current.submitted;
	uint16_t last = first;
	while (last < data_shards.size() and data_shards[last])
		++last;

	const bool frame_complete = last == data_shards.size() and
	                            not data_shards.empty() and
	                            data_shards.back()->timing_info;

	if (last > first)
	{
		if (frame_complete and not data_shards.front()->view_info)
		{
			// Nothing can be done with this frame: the decoder needs the view
			// info that rides the first shard to submit the layer at all.
			spdlog::warn("first shard has no view_info");
			return step::unusable;
		}

		if (last - first == 1)
		{
			std::span<const uint8_t> payload = data_shards[first]->payload;
			decoder_->push_data(std::span(&payload, 1), data_shards[first]->frame_idx, not frame_complete);
		}
		else
		{
			std::vector<std::span<const uint8_t>> payload;
			payload.reserve(last - first);
			for (size_t idx = first; idx < last; ++idx)
				payload.emplace_back(data_shards[idx]->payload);

			decoder_->push_data(payload, data_shards[first]->frame_idx, not frame_complete);
		}
		current.submitted = last;
	}

	if (not frame_complete)
		return step::wait;

	current.feedback.received_last_packet = instance.now();
	current.feedback.sent_to_decoder = current.feedback.received_last_packet;
	data_shard::timing_info_t timing_info = data_shards.back()->timing_info.value_or(data_shard::timing_info_t{});
	current.feedback.encode_begin = timing_info.encode_begin;
	current.feedback.encode_end = timing_info.encode_end;
	current.feedback.send_begin = timing_info.send_begin;
	current.feedback.send_end = timing_info.send_end;

	if (not data_shards.front()->view_info)
		return step::unusable;

	// Try to extract a frame
	decoder_->frame_completed(current.feedback, *data_shards.front()->view_info);

	send_feedback(current.feedback);

	// The window advances on `done`; advancing here as well would skip a frame.
	return step::done;
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
static int checks=0, failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::printf("FAIL:%d %s\n",__LINE__,#x);}}while(false)
static void add(shard_accumulator&a,uint64_t frame,uint16_t index,XrTime now,bool tail){
 auto*s=a.window.slot(frame,[](auto&){});CHECK(s);to_headset::video_stream_data_shard p{};
 p.stream_item_idx=2;p.frame_idx=frame;p.shard_idx=index;
 if(index==0)p.view_info=to_headset::video_stream_data_shard::view_info_t{};
 if(tail)p.timing_info=to_headset::video_stream_data_shard::timing_info_t{1,2,3,4};
 static std::array<uint8_t,4> bytes{1,2,3,4};p.payload=bytes;s->insert(std::move(p),now);
 if(s->complete())a.window.note_complete(frame);
 a.instance.value=now;a.pump(now);
}
static void test(bool enabled,bool newer){
 shard_accumulator a;auto scene=std::make_shared<scenes::stream>();a.weak_scene=scene;a.astc_deadline_enabled=enabled;
 // Frame0 has a confirmed interior hole1; its tail2 is known. Frame1 is complete.
 const XrTime first=1'000'000;add(a,0,0,first,false);add(a,0,2,first+1,true);
 if(newer){add(a,1,0,first+100,false);add(a,1,1,first+101,true);}
 CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());
 const XrTime due=first+2*scene->display_period_ns();
 a.instance.value=due-1;a.pump(due-1);CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());
 // Actual quiet polling entry spends both NACK rounds, but does not pump.
 a.instance.value=due;a.poll_nacks(due);
 a.instance.value=due+nack_quiet_period_ns;a.poll_nacks(a.instance.value);
 a.instance.value=due+100'000'000;a.poll_nacks(a.instance.value);
 CHECK(scene->requests.size()==2);CHECK(a.window.front().nack_rounds==2);
 CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());
 a.pump(a.instance.value);
 const bool release=enabled&&newer;
 CHECK(a.window.front_index()==(release?2u:0u));CHECK(a.decoder_->completed.size()==(release?1u:0u));
 if(release){CHECK(a.decoder_->completed[0]==1);CHECK(a.feedback_frames.size()==2);}
 std::printf("%d,%d,%lld,%llu,%zu\n",enabled,newer,(long long)due,(unsigned long long)a.window.front_index(),a.decoder_->completed.size());
}
int main(){std::puts("deadline_enabled,newer_complete,virtual_due_ns,front_after_explicit_pump,decoder_stub_completed");test(true,true);test(false,true);test(true,false);std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
