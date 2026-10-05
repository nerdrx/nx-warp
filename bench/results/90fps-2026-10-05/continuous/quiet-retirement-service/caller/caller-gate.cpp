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
namespace scenes { struct stream { std::vector<from_headset::nack> requests; XrDuration period=11'111'111; void send_nack(const from_headset::nack&n){requests.push_back(n);} XrDuration display_period_ns()const{return period;} }; }
struct fake_instance { XrTime value=1; XrTime now()const{return value;} };
struct fake_decoder {
 std::vector<uint64_t> completed; size_t bytes=0;
 void push_data(std::span<const std::span<const uint8_t>> payload,uint64_t,bool){for(auto p:payload)bytes+=p.size();}
 void frame_completed(const from_headset::feedback&fb,const to_headset::video_stream_data_shard::view_info_t&){completed.push_back(fb.frame_index);}
};
class shard_accumulator {
public:
 using shard_set=wivrn::shard_set; using data_shard=to_headset::video_stream_data_shard;
 	using window_t = wivrn::frame_window<shard_set, 6, 3>;
 window_t window{shard_set{2}}; fake_instance instance;
 std::shared_ptr<fake_decoder> decoder_=std::make_shared<fake_decoder>();
 std::weak_ptr<scenes::stream> weak_scene; bool astc_deadline_enabled=false; bool nxastc_codec=true;
 std::vector<uint16_t> nack_scratch; uint64_t nack_requests=0,nack_shards=0;
 std::atomic<uint64_t> nack_shards_total=0;
 void report_nacks(XrTime){}
 bool is_nxastc_codec() const{return nxastc_codec;}
 std::optional<XrTime> next_nack_deadline(XrTime); std::optional<XrTime> next_poll_deadline(XrTime);
 void poll_nacks(XrTime); void try_nack(XrTime); void pump(XrTime);
 window_t::step try_submit_front(shard_set&);
 std::vector<uint64_t> feedback_frames;
 void send_feedback(from_headset::feedback&fb){feedback_frames.push_back(fb.frame_index);}
};
static void debug_why_not_sent(const shard_accumulator::shard_set&){}
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

std::optional<XrTime> shard_accumulator::next_poll_deadline(XrTime now)
{
	auto due = next_nack_deadline(now);
	if (not nxastc_codec or not astc_deadline_enabled or window.front().empty() or
	    window.front().complete() or not window.has_newer_complete_than_front())
		return due;

	const auto scene = weak_scene.lock();
	if (not scene)
		return due;
	const XrTime first = window.front().feedback.received_first_packet;
	const XrDuration period = scene->display_period_ns();
	if (first <= 0 or now < first or period <= 0 or
	    period > (std::numeric_limits<XrTime>::max() - first) / 2)
		return due;

	const XrTime retirement = first + period * 2;
	return due ? std::min(*due, retirement) : retirement;
}

void shard_accumulator::poll_nacks(XrTime now)
{
	if (nxastc_codec)
	{
		// Quiet traffic must service the same opt-in deadline as shard arrivals.
		if (astc_deadline_enabled)
			pump(now);
		try_nack(now);
	}
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
}
using namespace wivrn;
static int checks=0, failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::printf("FAIL:%d %s\n",__LINE__,#x);}}while(false)
static void add(shard_accumulator&a,uint64_t frame,uint16_t index,XrTime now,bool tail){
 auto*s=a.window.slot(frame,[](auto&){});CHECK(s);if(!s)return;
 to_headset::video_stream_data_shard p{};p.stream_item_idx=2;p.frame_idx=frame;p.shard_idx=index;
 if(index==0)p.view_info=to_headset::video_stream_data_shard::view_info_t{};
 if(tail)p.timing_info=to_headset::video_stream_data_shard::timing_info_t{1,2,3,4};
 static std::array<uint8_t,4> bytes{1,2,3,4};p.payload=bytes;s->insert(std::move(p),now);
 if(s->complete()) a.window.note_complete(frame);
 a.instance.value=now;a.pump(now);
}
static void setup(shard_accumulator&a,std::shared_ptr<scenes::stream>&scene,bool deadline=true,XrTime t=1'000'000'000){
 scene=std::make_shared<scenes::stream>();a.weak_scene=scene;a.astc_deadline_enabled=deadline;
 // Old frame has an interior hole and a real terminal shard; next frame is complete.
 add(a,0,0,t,false);add(a,0,2,t+1,true);add(a,1,0,t+2,false);add(a,1,1,t+3,true);
 CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());
}
static void service_two_nack_rounds(shard_accumulator&a,XrTime t){
 const XrTime quiet=nack_quiet_period_ns;
 const XrTime active=t+1;
 a.instance.value=active+quiet;a.poll_nacks(a.instance.value);CHECK(a.nack_requests==1);
 a.instance.value=active+2*quiet;a.poll_nacks(a.instance.value);CHECK(a.nack_requests==2);
 CHECK(a.window.front().nack_rounds==2);
}
static void test_retirement(){
 shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true);const XrTime t=1'000'000'000;const XrDuration p=s->period;
 auto before=a.next_poll_deadline(t+3);CHECK(before&&*before==t+1+nack_quiet_period_ns);
 service_two_nack_rounds(a,t);auto due=a.next_poll_deadline(t+3+2*nack_quiet_period_ns);CHECK(due&&*due==t+2*p);
 a.instance.value=*due-1;a.poll_nacks(a.instance.value);CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());CHECK(a.next_poll_deadline(*due-1)==due);
 a.instance.value=*due;a.poll_nacks(*due);CHECK(a.window.front_index()==2);CHECK(a.decoder_->completed.size()==1);CHECK(a.decoder_->completed[0]==1);
 CHECK(a.feedback_frames.size()==2);CHECK(!a.next_poll_deadline(*due));
 // A later quiet poll cannot repeat retirement or redeliver the already-pumped frame.
 a.instance.value=*due+100'000'000;a.poll_nacks(a.instance.value);CHECK(a.window.front_index()==2);CHECK(a.decoder_->completed.size()==1);
}
static void test_controls(){
 const XrTime t=2'000'000'000;
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,false,t);CHECK(a.next_poll_deadline(t+2*s->period)==a.next_nack_deadline(t+2*s->period));a.instance.value=t+2*s->period;a.poll_nacks(a.instance.value);CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);application::current.shard_retransmit=false;CHECK(a.next_poll_deadline(t+3)==std::optional<XrTime>(t+2*s->period));a.instance.value=t+2*s->period;a.poll_nacks(a.instance.value);CHECK(a.window.front_index()==2);CHECK(a.decoder_->completed.size()==1);application::current.shard_retransmit=true;}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);a.nxastc_codec=false;CHECK(!a.next_poll_deadline(t));}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);s.reset();a.weak_scene.reset();CHECK(!a.next_poll_deadline(t+3));}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);s->period=0;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));s->period=-1;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));s->period=std::numeric_limits<XrDuration>::max();CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);auto*front=&a.window.front();front->feedback.received_first_packet=0;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));front->feedback.received_first_packet=-1;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);auto*front=&a.window.front();front->feedback.received_first_packet=std::numeric_limits<XrTime>::max()-1;CHECK(a.next_poll_deadline(std::numeric_limits<XrTime>::max())==a.next_nack_deadline(std::numeric_limits<XrTime>::max()));}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s;setup(a,s,true,t);auto*front=&a.window.front();front->feedback.received_first_packet=t+5;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s=std::make_shared<scenes::stream>();a.weak_scene=s;a.astc_deadline_enabled=true;add(a,0,0,t,false);add(a,0,2,t+1,true);CHECK(!a.window.has_newer_complete_than_front());auto due=a.next_poll_deadline(t+1);CHECK(due&&*due==t+1+nack_quiet_period_ns);}
 {shard_accumulator a;std::shared_ptr<scenes::stream>s=std::make_shared<scenes::stream>();a.weak_scene=s;a.astc_deadline_enabled=true;add(a,0,0,t,false);add(a,0,2,t+1,true);CHECK(!a.window.has_newer_complete_than_front());auto due=a.next_poll_deadline(t+1);CHECK(due&&*due==t+1+nack_quiet_period_ns);}
 {const auto future=wivrn::nack_poll_timeout(10'000'000,10'000'001, std::chrono::milliseconds(100));CHECK(future.count()==1);const auto rounded=wivrn::nack_poll_timeout(10'000'000,12'500'001,std::chrono::milliseconds(100));CHECK(rounded.count()==3);CHECK(wivrn::nack_poll_timeout(12'500'001,12'500'001,std::chrono::milliseconds(100)).count()==0);CHECK(wivrn::nack_poll_timeout(10'000'000,{},std::chrono::milliseconds(100)).count()==100);}
}
int method_gate_main(){std::puts("gate=actual extracted accumulator methods; virtual time; excludes constructor, push/process_packets, network thread, decoder/XR");test_retirement();test_controls();std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}

#include <functional>
#include <shared_mutex>
#include <stdexcept>
namespace caller_projection {
static bool enabled=true;
static bool recovery_poll_enabled(){return enabled;}
namespace scenes { struct stream; }
struct session {
 fake_instance &clock;
 std::vector<int64_t> waits;
 std::function<void()> stop;
 template<class Visitor,class Supplier> void poll(Visitor&,std::chrono::milliseconds maximum,Supplier&& supplier){
  auto wait=std::min(maximum,supplier());waits.push_back(wait.count());
  clock.value+=wait.count()*1'000'000;
  if(waits.size()==2)stop();
  if(waits.size()>2)throw std::runtime_error("unbounded projected caller");
 }
 template<class Visitor> void poll(Visitor&v,std::chrono::milliseconds wait){poll(v,wait,[wait]{return wait;});}
};
namespace scenes {
struct stream {
 fake_instance &instance;
 std::shared_mutex decoder_mutex;
 struct item {std::shared_ptr<shard_accumulator> decoder;};
 std::array<item,1> decoders;
 enum class state {streaming,shutdown};state state_=state::streaming;
 std::unique_ptr<session> network_session;
 stream(std::shared_ptr<shard_accumulator>a):instance(a->instance),decoders{item{a}},network_session(std::make_unique<session>(session{instance,{},{}})){network_session->stop=[this]{state_=state::shutdown;};}
 bool try_seamless_reconnect(){return false;}
 void exit(){state_=state::shutdown;}
 void process_packets();
};
}
void scenes::stream::process_packets()
{
#ifdef __ANDROID__
	application::instance().setup_jni();
#endif
	const bool use_recovery_poll = recovery_poll_enabled();
	while (state_ != state::shutdown)
	{
		try
		{
			// Short enough that the path selector, evaluated at the end of every
			// poll, still reacts within a few hundred ms once the primary path
			// has gone completely silent
			if (not use_recovery_poll)
				network_session->poll(*this, std::chrono::milliseconds(100));
			else
			{
				auto timeout = [this]() {
					const auto now = instance.now();
					auto wait = std::chrono::milliseconds(100);
					std::shared_lock lock(decoder_mutex);
					for (auto & item: decoders)
						if (item.decoder and item.decoder->is_nxastc_codec())
							wait = std::min(wait, wivrn::nack_poll_timeout(now, item.decoder->next_poll_deadline(now), wait));
					return wait;
				};
				network_session->poll(*this, std::chrono::milliseconds(100), timeout);
				if (state_ != state::shutdown)
				{
					const XrTime now = instance.now();
					std::shared_lock lock(decoder_mutex);
					for (auto & item: decoders)
						if (item.decoder and item.decoder->is_nxastc_codec())
							item.decoder->poll_nacks(now);
				}
			}
		}
		catch (std::exception & e)
		{
			spdlog::info("Exception in network thread: {}", e.what());

			// Seamless reconnect: hold the stream scene alive and re-handshake in the
			// background rather than dropping straight to the lobby. Returns false (and
			// falls through to exit()) when the feature is off, the window is exhausted,
			// or the user cancels, i.e. exactly the old behaviour.
			if (not try_seamless_reconnect())
			{
				spdlog::info("Network thread exiting");
				exit();
			}
		}
	}
}
}
static void caller_case(bool recovery,bool deadline,bool native){
 auto a=std::make_shared<shard_accumulator>();std::shared_ptr<wivrn::scenes::stream>s;
 setup(*a,s,deadline);service_two_nack_rounds(*a,1'000'000'000);a->nxastc_codec=native;
 caller_projection::enabled=recovery;caller_projection::scenes::stream loop(a);loop.process_packets();
 auto&waits=loop.network_session->waits;CHECK(waits.size()==2);
 CHECK(waits[0]==(recovery&&deadline&&native?18:100));CHECK(waits[1]==100);
 CHECK(a->window.front_index()==(recovery&&deadline&&native?2u:0u));
 CHECK(a->decoder_->completed.size()==(recovery&&deadline&&native?1u:0u));CHECK(a->nack_requests==2);
 std::printf("caller,%d,%d,%d,%lld,%llu,%zu\n",recovery,deadline,native,(long long)waits[0],(unsigned long long)a->window.front_index(),a->decoder_->completed.size());
}
int main(){
 int base_result=method_gate_main();if(base_result)return base_result;
 checks=0;failures=0;
 std::puts("kind,recovery_poll,deadline,native,first_wait_ms,front,decoder_stub_completed");
 caller_case(true,true,true);caller_case(false,true,true);caller_case(true,false,true);caller_case(true,true,false);
 std::printf("caller_checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
