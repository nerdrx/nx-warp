#include <functional>
#include <chrono>
#include <cstdlib>
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
using namespace wivrn::to_headset;
namespace application { struct config { bool shard_retransmit=true; }; inline config current{}; inline config& get_config(){return current;} }
namespace scenes { struct stream { std::vector<from_headset::nack> requests; XrDuration period=11'111'111; void send_nack(const from_headset::nack&n){requests.push_back(n);} XrDuration display_period_ns()const{return period;} }; }
struct fake_instance { XrTime value=1; bool realtime=false; XrTime now()const{return realtime?std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count():value;} };
struct fake_decoder { std::function<void()> on_complete;
 std::vector<uint64_t> completed; std::vector<std::vector<uint8_t>> completed_payloads; std::vector<uint8_t> pending_payload; std::vector<size_t> completed_bytes; uint64_t pending_frame=UINT64_MAX; size_t pending_bytes=0;
 void push_data(std::span<const std::span<const uint8_t>> payload,uint64_t frame,bool){if(frame!=pending_frame){pending_frame=frame;pending_bytes=0;pending_payload.clear();}for(auto p:payload){pending_bytes+=p.size();pending_payload.insert(pending_payload.end(),p.begin(),p.end());}}
 void frame_completed(const from_headset::feedback&fb,const to_headset::video_stream_data_shard::view_info_t&){completed.push_back(fb.frame_index);completed_bytes.push_back(pending_bytes);completed_payloads.push_back(pending_payload);if(on_complete)on_complete();}
};
class shard_accumulator {
public:
 using shard_set=wivrn::shard_set; using data_shard=to_headset::video_stream_data_shard;
 	using window_t = wivrn::frame_window<shard_set, 6, 3>;
 window_t window{shard_set{0}}; fake_instance instance;
 std::shared_ptr<fake_decoder> decoder_=std::make_shared<fake_decoder>();
 std::weak_ptr<wivrn::scenes::stream> weak_scene; bool astc_deadline_enabled=false; bool nxastc_codec=true;
 std::vector<uint16_t> nack_scratch; uint64_t nack_requests=0,nack_shards=0;
 std::atomic<uint64_t> nack_shards_total=0;
 void report_nacks(XrTime){} bool is_nxastc_codec() const{return nxastc_codec;}
 void push_shard(video_stream_data_shard&&); void drain_parity(shard_set&){}
 std::optional<XrTime> next_nack_deadline(XrTime); std::optional<XrTime> next_poll_deadline(XrTime);
 void poll_nacks(XrTime); void try_nack(XrTime); void pump(XrTime);
 window_t::step try_submit_front(shard_set&);
 std::vector<uint64_t> feedback_frames;
 void send_feedback(from_headset::feedback&fb){feedback_frames.push_back(fb.frame_index);}
};
static void debug_why_not_sent(const shard_accumulator::shard_set&){}
void shard_accumulator::push_shard(video_stream_data_shard && shard)
{
	// Not truncated to 8 bits: a stream that is silent for a while, as the quad
	// layer stream is whenever no layer is promoted, comes back with a gap of any
	// size, and a gap that happened to be a multiple of 256 would look like no gap
	// at all and file the new frame's shards under the old frame index.
	const uint64_t frame_idx = shard.frame_idx;

	shard_set * set = window.slot(frame_idx, [this](shard_set & s) {
		debug_why_not_sent(s);
		send_feedback(s.feedback);
	});

	if (not set)
	{
		// Older than anything still being reassembled. With two paths in play this
		// is also what a shard that lost a race by more than the window looks like.
		spdlog::info("Drop shard for old frame {} (oldest {})", frame_idx, window.front_index());
		return;
	}

	const XrTime now = instance.now();
	set->insert(std::move(shard), now);
	// The shard that just landed may have been the last one a group was short of
	// bar one, which is the point at which its parity becomes usable.
	drain_parity(*set);

	if (set->complete())
		window.note_complete(frame_idx);

	// After the parity drain, never before it: what the parity is about to rebuild
	// must not be asked for again.
	try_nack(now);

	// Only the oldest frame is ever handed to the decoder, and the pump is what
	// does it; a shard for a newer one can only ever change whether the oldest is
	// still worth waiting for.
	pump(now);
}
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
static void setup(shard_accumulator&a,std::shared_ptr<wivrn::scenes::stream>&scene,bool deadline=true,XrTime t=1'000'000'000){
 scene=std::make_shared<wivrn::scenes::stream>();a.weak_scene=scene;a.astc_deadline_enabled=deadline;
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
 shard_accumulator a;std::shared_ptr<wivrn::scenes::stream>s;setup(a,s,true);const XrTime t=1'000'000'000;const XrDuration p=s->period;
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
 {shard_accumulator a;std::shared_ptr<wivrn::scenes::stream>s;setup(a,s,false,t);CHECK(a.next_poll_deadline(t+2*s->period)==a.next_nack_deadline(t+2*s->period));a.instance.value=t+2*s->period;a.poll_nacks(a.instance.value);CHECK(a.window.front_index()==0);CHECK(a.decoder_->completed.empty());}
 {shard_accumulator a;std::shared_ptr<wivrn::scenes::stream>s;setup(a,s,true,t);application::current.shard_retransmit=false;CHECK(a.next_poll_deadline(t+3)==std::optional<XrTime>(t+2*s->period));a.instance.value=t+2*s->period;a.poll_nacks(a.instance.value);CHECK(a.window.front_index()==2);CHECK(a.decoder_->completed.size()==1);application::current.shard_retransmit=true;}
 {shard_accumulator a;std::shared_ptr<wivrn::scenes::stream>s;setup(a,s,true,t);a.nxastc_codec=false;CHECK(!a.next_poll_deadline(t));}
 {shard_accumulator a;std::shared_ptr<wivrn::scenes::stream>s;setup(a,s,true,t);s.reset();a.weak_scene.reset();CHECK(!a.next_poll_deadline(t+3));}
 {shard_accumulator a;std::shared_ptr<wivrn::scenes::stream>s;setup(a,s,true,t);s->period=0;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));s->period=-1;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));s->period=std::numeric_limits<XrDuration>::max();CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));}
 {shard_accumulator a;std::shared_ptr<wivrn::scenes::stream>s;setup(a,s,true,t);auto*front=&a.window.front();front->feedback.received_first_packet=0;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));front->feedback.received_first_packet=-1;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));}
 {shard_accumulator a;std::shared_ptr<wivrn::scenes::stream>s;setup(a,s,true,t);auto*front=&a.window.front();front->feedback.received_first_packet=std::numeric_limits<XrTime>::max()-1;CHECK(a.next_poll_deadline(std::numeric_limits<XrTime>::max())==a.next_nack_deadline(std::numeric_limits<XrTime>::max()));}
 {shard_accumulator a;std::shared_ptr<wivrn::scenes::stream>s;setup(a,s,true,t);auto*front=&a.window.front();front->feedback.received_first_packet=t+5;CHECK(a.next_poll_deadline(t+3)==a.next_nack_deadline(t+3));}
 {shard_accumulator a;std::shared_ptr<wivrn::scenes::stream>s=std::make_shared<wivrn::scenes::stream>();a.weak_scene=s;a.astc_deadline_enabled=true;add(a,0,0,t,false);add(a,0,2,t+1,true);CHECK(!a.window.has_newer_complete_than_front());auto due=a.next_poll_deadline(t+1);CHECK(due&&*due==t+1+nack_quiet_period_ns);}
 {shard_accumulator a;std::shared_ptr<wivrn::scenes::stream>s=std::make_shared<wivrn::scenes::stream>();a.weak_scene=s;a.astc_deadline_enabled=true;add(a,0,0,t,false);add(a,0,2,t+1,true);CHECK(!a.window.has_newer_complete_than_front());auto due=a.next_poll_deadline(t+1);CHECK(due&&*due==t+1+nack_quiet_period_ns);}
 {const auto future=wivrn::nack_poll_timeout(10'000'000,10'000'001, std::chrono::milliseconds(100));CHECK(future.count()==1);const auto rounded=wivrn::nack_poll_timeout(10'000'000,12'500'001,std::chrono::milliseconds(100));CHECK(rounded.count()==3);CHECK(wivrn::nack_poll_timeout(12'500'001,12'500'001,std::chrono::milliseconds(100)).count()==0);CHECK(wivrn::nack_poll_timeout(10'000'000,{},std::chrono::milliseconds(100)).count()==100);}
}
int accumulator_gate_main(){std::puts("gate=actual extracted accumulator methods; virtual time; excludes constructor, push/process_packets, network thread, decoder/XR");test_retirement();test_controls();std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}

#include "wivrn_sockets.h"
#include "wivrn_packets.h"
#include "nack_deadline.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <poll.h>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <spdlog/spdlog.h>
using namespace std::chrono_literals;
using namespace wivrn;
struct selector_stub { bool control_up() const { return true; } };
class poll_projection {
public:
 using control_socket_t=typed_socket<TCP, to_headset::packets, from_headset::packets>;
 using stream_socket_t=typed_socket<UDP, to_headset::packets, from_headset::packets>;
 stream_socket_t stream; control_socket_t control; control_socket_t secondary{-1};
 std::atomic<uint64_t> bytes_received_{0}; std::shared_mutex secondary_mutex;
 uint64_t secondary_generation=0; std::atomic<bool> secondary_up{false}; selector_stub selector;
 poll_projection(stream_socket_t &&s,control_socket_t &&c):stream(std::move(s)),control(std::move(c)){}
 void poll_secondary_pending(auto&&){}
 void on_primary_received(bool){}
 bool on_control_send_error(const std::exception&){return false;}
 void on_stream_send_error(const std::exception&){}
 void drop_secondary(std::string_view) { secondary_up=false; }
 void update_paths(){}
 	template <typename T, typename TimeoutSupplier>
	int poll(T && visitor, std::chrono::milliseconds max_timeout, TimeoutSupplier && timeout_supplier)
	{
		pollfd fds[3] = {};
		fds[0].events = POLLIN;
		fds[0].fd = stream.get_fd();
		fds[1].events = POLLIN;
		// A control socket that is known broken reports POLLERR on every poll:
		// leaving it in would spin the network thread
		fds[1].fd = selector.control_up() ? control.get_fd() : -1;

		// Malformed datagrams are dropped, only the control socket is fatal
		while (auto packet = stream.receive_pending_lossy(&bytes_received_))
		{
			on_primary_received(false);
			std::visit(std::forward<T>(visitor), std::move(*packet));
		}
		while (selector.control_up())
		{
			std::optional<to_headset::packets> packet;
			try
			{
				packet = control.receive_pending(&bytes_received_);
			}
			catch (const std::exception & e)
			{
				if (not on_control_send_error(e))
					throw;
				break;
			}

			if (not packet)
				break;

			on_primary_received(true);
			std::visit(std::forward<T>(visitor), std::move(*packet));
		}
		poll_secondary_pending(std::forward<T>(visitor));

		uint64_t secondary_gen = 0;
		{
			std::shared_lock lock(secondary_mutex);
			fds[2].events = POLLIN;
			fds[2].fd = secondary_up ? secondary.get_fd() : -1;
			secondary_gen = secondary_generation;
		}

		const auto timeout = std::min(max_timeout, timeout_supplier());
		int r = ::poll(fds, std::size(fds), timeout.count());
		if (r < 0)
			throw std::system_error(errno, std::system_category());

		if (fds[0].revents & (POLLHUP | POLLERR))
		{
			// A connected UDP socket raises POLLERR for a latched error, most
			// often an ICMP unreachable while the link is down. Draining it
			// makes the socket usable again the moment the link is back, so
			// this must never be a session teardown.
			on_stream_send_error(std::runtime_error("poll reported an error"));
		}

		if (fds[1].revents & (POLLHUP | POLLERR))
		{
			if (not on_control_send_error(std::runtime_error("Error on control socket")))
				throw std::runtime_error("Error on control socket");
		}

		if (fds[2].revents & (POLLHUP | POLLERR | POLLNVAL))
		{
			drop_secondary("socket closed by peer");
		}
		else if (fds[2].revents & POLLIN)
		{
			std::optional<to_headset::packets> packet;
			std::string error;
			{
				std::shared_lock lock(secondary_mutex);
				// The path may have been replaced while polling
				if (secondary_up and secondary_generation == secondary_gen)
				{
					try
					{
						packet = secondary.receive(&bytes_received_);
					}
					catch (std::exception & e)
					{
						error = e.what();
					}
				}
			}

			if (not error.empty())
				drop_secondary(error);
			else if (packet)
				std::visit(std::forward<T>(visitor), std::move(*packet));
		}

		if (fds[0].revents & POLLIN)
		{
			std::optional<to_headset::packets> packet;
			try
			{
				packet = stream.receive_lossy(&bytes_received_);
			}
			catch (const std::exception & e)
			{
				// Same as above: recvmmsg reports the latched error
				on_stream_send_error(e);
			}

			if (packet)
			{
				on_primary_received(false);
				std::visit(std::forward<T>(visitor), std::move(*packet));
			}
		}

		if (fds[1].revents & POLLIN)
		{
			std::optional<to_headset::packets> packet;
			try
			{
				packet = control.receive(&bytes_received_);
			}
			catch (const std::exception & e)
			{
				if (not on_control_send_error(e))
					throw;
			}

			if (packet)
			{
				on_primary_received(true);
				std::visit(std::forward<T>(visitor), std::move(*packet));
			}
		}

		if (uint64_t dropped = stream.take_dropped_datagrams())
			spdlog::warn("Dropped {} invalid datagram(s) on the stream socket ({} total)", dropped, stream.dropped_datagrams());

		update_paths();

		return r;
	}
};

using udp_sender_t=typed_socket<UDP, from_headset::packets, to_headset::packets>;
using tcp_sender_t=typed_socket<TCP, from_headset::packets, to_headset::packets>;
static std::pair<poll_projection::stream_socket_t,udp_sender_t> udp_pair(){
 wivrn::UDP rx; sockaddr_in6 bindaddr{};bindaddr.sin6_family=AF_INET6;bindaddr.sin6_addr=in6addr_loopback;bindaddr.sin6_port=0;rx.bind(bindaddr);
 sockaddr_in6 actual{};socklen_t n=sizeof(actual);getsockname(rx.get_fd(),(sockaddr*)&actual,&n);
 wivrn::UDP tx;tx.connect(in6addr_loopback,ntohs(actual.sin6_port));
 return {poll_projection::stream_socket_t(std::move(rx)),udp_sender_t(std::move(tx))};
}
static std::pair<poll_projection::control_socket_t,tcp_sender_t> tcp_pair(){
 TCPListener listener(0);sockaddr_in6 addr{};socklen_t n=sizeof(addr);getsockname(listener.get_fd(),(sockaddr*)&addr,&n);
 wivrn::TCP tx(in6addr_loopback,ntohs(addr.sin6_port));auto [rx,peer]=listener.accept<wivrn::TCP>();
 return {poll_projection::control_socket_t(std::move(rx)),tcp_sender_t(std::move(tx))};
}


using namespace std::chrono_literals;
namespace caller_projection { inline bool enabled=true; inline bool recovery_poll_enabled(){return enabled;} }
using udp_sender_t=typed_socket<UDP,from_headset::packets,to_headset::packets>;
using tcp_sender_t=typed_socket<TCP,from_headset::packets,to_headset::packets>;
class actual_session:public poll_projection{
public:
 fake_instance&clock;udp_sender_t udp_peer;tcp_sender_t tcp_peer;std::vector<int>requested_ms;std::vector<int>poll_returns;std::vector<int64_t>elapsed_ns;std::function<void()> stop_after_two;size_t calls=0;size_t max_polls_before_stop=2;
 actual_session(poll_projection::stream_socket_t&&r,poll_projection::control_socket_t&&c,udp_sender_t&&u,tcp_sender_t&&t,fake_instance&cl):poll_projection(std::move(r),std::move(c)),clock(cl),udp_peer(std::move(u)),tcp_peer(std::move(t)){}
 template<class V,class S>int poll(V&&v,std::chrono::milliseconds maximum,S&&supplier){auto begin=std::chrono::steady_clock::now();int wanted=0;int ret=poll_projection::poll(std::forward<V>(v),maximum,[&]{auto w=std::min(maximum,supplier());wanted=int(w.count());return w;});auto end=std::chrono::steady_clock::now();auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count();poll_returns.push_back(ret);requested_ms.push_back(wanted);elapsed_ns.push_back(ns);clock.value+=ns;if(++calls==max_polls_before_stop&&stop_after_two)stop_after_two();return ret;}
 template<class V>int poll(V&&v,std::chrono::milliseconds maximum){return poll(std::forward<V>(v),maximum,[maximum]{return maximum;});}
};
namespace caller_projection { namespace scenes {
class stream {
public:
 fake_instance&instance;std::shared_mutex decoder_mutex;struct item{std::shared_ptr<shard_accumulator>decoder;};std::array<item,1>decoders;std::vector<std::pair<uint64_t,uint16_t>> arrivals;XrTime first_receipt=0;
 enum class state{streaming,shutdown};state state_=state::streaming;std::unique_ptr<actual_session>network_session;
 stream(std::shared_ptr<shard_accumulator>a):instance(a->instance),decoders{item{a}}{auto[u,up]=udp_pair();auto[c,cp]=tcp_pair();network_session=std::make_unique<actual_session>(std::move(u),std::move(c),std::move(up),std::move(cp),instance);network_session->stop_after_two=[this]{state_=state::shutdown;};a->decoder_->on_complete=[this]{state_=state::shutdown;};}
 void operator()(to_headset::video_stream_data_shard&& shard){arrivals.emplace_back(shard.frame_idx,shard.shard_idx);if(shard.stream_item_idx==0&&decoders[0].decoder) {decoders[0].decoder->push_shard(std::move(shard));if(first_receipt==0&&decoders[0].decoder->window.front_index()==0)first_receipt=decoders[0].decoder->window.front().feedback.received_first_packet;}}
 template<class T>void operator()(T&&){}
 bool try_seamless_reconnect(){return false;}void exit(){state_=state::shutdown;}void process_packets();
};}}
void caller_projection::scenes::stream::process_packets()
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
static int64_t host_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static void integrated_case(bool recovery,bool deadline,bool native){
 auto a=std::make_shared<shard_accumulator>();std::shared_ptr<wivrn::scenes::stream>scene;XrTime start=host_ns()-10'000'000;setup(*a,scene,deadline,start);service_two_nack_rounds(*a,start);a->nxastc_codec=native;a->instance.value=std::max<XrTime>(a->instance.value,host_ns());caller_projection::enabled=recovery;caller_projection::scenes::stream loop(a);loop.process_packets();auto&s=*loop.network_session;bool expected=recovery&&deadline&&native;CHECK(a->window.front_index()==(expected?2u:0u));CHECK(a->decoder_->completed.size()==(expected?1u:0u));CHECK(a->nack_requests==2);CHECK(s.bytes_received_==0);CHECK(std::all_of(s.poll_returns.begin(),s.poll_returns.end(),[](int r){return r==0;}));CHECK(!s.requested_ms.empty());if(s.requested_ms.empty())return;if(expected){CHECK(s.requested_ms[0]>=0&&s.requested_ms[0]<100);CHECK(s.elapsed_ns[0]>=0);CHECK(loop.state_==caller_projection::scenes::stream::state::shutdown);}else{CHECK(s.requested_ms[0]==100);CHECK(s.requested_ms.size()==2);CHECK(loop.state_==caller_projection::scenes::stream::state::shutdown);}std::printf("case,%d,%d,%d,wait_ms=%d,elapsed_ns=%lld,polls=%zu,front=%llu,completed=%zu,received_bytes=%llu,poll_return=%d\n",recovery,deadline,native,s.requested_ms[0],(long long)s.elapsed_ns[0],s.calls,(unsigned long long)a->window.front_index(),a->decoder_->completed.size(),(unsigned long long)s.bytes_received_.load(),s.poll_returns.front());}

using data_shard=wivrn::to_headset::video_stream_data_shard;
static std::array<std::array<std::array<uint8_t,5>,3>,2> wire_bytes{};
static data_shard make_wire_shard(uint64_t frame,uint16_t index,bool terminal=false){data_shard d{};d.stream_item_idx=0;d.frame_idx=frame;d.shard_idx=index;auto&b=wire_bytes[frame][index];b.fill(uint8_t(0x20+frame*4+index));d.payload=b;if(index==0)d.view_info=data_shard::view_info_t{.display_time=42};if(terminal)d.timing_info=data_shard::timing_info_t{1,2,3,4};return d;}
static void send_initial(caller_projection::scenes::stream&loop,bool timely_repair=false){auto&peer=loop.network_session->udp_peer;peer.send(make_wire_shard(0,0));peer.send(make_wire_shard(0,2,true));peer.send(make_wire_shard(1,0));peer.send(make_wire_shard(1,1,true));if(timely_repair)peer.send(make_wire_shard(0,1));}
static void test_real_udp_arrivals(){
 checks=0;failures=0;std::puts("typed UDP -> exact push_shard -> quiet deadline service; steady_clock receipt times; fake XR/decoder/scene; direct timely-repair injection");
 auto a=std::make_shared<shard_accumulator>();a->instance.realtime=true;a->astc_deadline_enabled=true;auto scene=std::make_shared<wivrn::scenes::stream>();scene->period=11'111'111;a->weak_scene=scene;caller_projection::enabled=true;caller_projection::scenes::stream loop(a);loop.network_session->max_polls_before_stop=10;send_initial(loop);auto began=host_ns();loop.process_packets();auto ended=host_ns();auto&sock=*loop.network_session;CHECK(sock.bytes_received_.load()>0);CHECK(loop.arrivals.size()==4);CHECK(loop.arrivals[0].first==0&&loop.arrivals[0].second==0);CHECK(loop.arrivals[1].first==0&&loop.arrivals[1].second==2);CHECK(loop.arrivals[2].first==1&&loop.arrivals[2].second==0);CHECK(loop.arrivals[3].first==1&&loop.arrivals[3].second==1);auto first=loop.first_receipt;CHECK(first>=began&&first<=ended);CHECK(a->feedback_frames==std::vector<uint64_t>({0,1}));CHECK(a->decoder_->completed==std::vector<uint64_t>{1});CHECK(a->decoder_->completed_bytes==std::vector<size_t>{10});CHECK(a->decoder_->completed_payloads.size()==1&&a->decoder_->completed_payloads[0]==std::vector<uint8_t>({36,36,36,36,36,37,37,37,37,37}));CHECK(scene->requests.size()==2);CHECK(a->window.front_index()==2);CHECK(loop.state_==caller_projection::scenes::stream::state::shutdown);std::printf("quiet bytes=%llu first_ns=%lld polls=%zu nack_requests=%zu feedback_count=%zu decoded=%llu payload_bytes=%zu\n",(unsigned long long)sock.bytes_received_.load(),(long long)first,sock.calls,scene->requests.size(),a->feedback_frames.size(),(unsigned long long)a->decoder_->completed.at(0),a->decoder_->completed_bytes.at(0));
 // A late shard travels through the same UDP receiver and exact push_shard; its old index is ignored.
 auto late_bytes=sock.bytes_received_.load();sock.udp_peer.send(make_wire_shard(0,1));sock.poll(loop,10ms,[&]{return 10ms;});CHECK(sock.bytes_received_.load()>late_bytes);CHECK(loop.arrivals.size()==5&&loop.arrivals.back().first==0&&loop.arrivals.back().second==1);CHECK(a->window.front_index()==2);CHECK(a->decoder_->completed==std::vector<uint64_t>{1});
 // With deadline retirement disabled, real arrivals do not release the newer complete frame during silence.
 auto b=std::make_shared<shard_accumulator>();b->instance.realtime=true;b->astc_deadline_enabled=false;auto sc=std::make_shared<wivrn::scenes::stream>();sc->period=11'111'111;b->weak_scene=sc;caller_projection::scenes::stream control(b);control.network_session->max_polls_before_stop=4;send_initial(control);auto control_start=host_ns();control.process_packets();CHECK(host_ns()-control_start>=2*sc->period);CHECK(control.network_session->bytes_received_.load()>0);CHECK(b->window.front_index()==0);CHECK(b->decoder_->completed.empty());CHECK(control.arrivals.size()==4);
 // A missing interior shard arriving before retirement completes and delivers both frames in order.
 auto c=std::make_shared<shard_accumulator>();c->instance.realtime=true;c->astc_deadline_enabled=true;auto cs=std::make_shared<wivrn::scenes::stream>();cs->period=11'111'111;c->weak_scene=cs;caller_projection::scenes::stream timely(c);timely.network_session->max_polls_before_stop=10;send_initial(timely,true);timely.process_packets();CHECK(timely.network_session->bytes_received_.load()>0);CHECK(c->decoder_->completed==std::vector<uint64_t>({0,1}));CHECK(c->feedback_frames==std::vector<uint64_t>({0,1}));CHECK(c->decoder_->completed_bytes==std::vector<size_t>({15,10}));CHECK(c->decoder_->completed_payloads.size()==2&&c->decoder_->completed_payloads[0]==std::vector<uint8_t>({32,32,32,32,32,33,33,33,33,33,34,34,34,34,34}));CHECK(c->decoder_->completed_payloads[1]==a->decoder_->completed_payloads[0]);
 std::printf("case,quiet,%llu,%zu,%zu\n",(unsigned long long)sock.bytes_received_.load(),a->decoder_->completed.size(),sock.calls);std::printf("case,deadline_off,%llu,%zu,%zu\n",(unsigned long long)control.network_session->bytes_received_.load(),b->decoder_->completed.size(),control.network_session->calls);std::printf("case,timely_direct_repair,%llu,%zu,%zu\n",(unsigned long long)timely.network_session->bytes_received_.load(),c->decoder_->completed.size(),timely.network_session->calls);std::printf("checks=%d failures=%d\n",checks,failures);if(failures)std::exit(1);
}

int main(){test_real_udp_arrivals();return 0;}
