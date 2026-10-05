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

/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "fec.h"
#include "frame_window.h"
#include "nack_deadline.h"
#include "shard_set.h"
#include "shard_history.h"
#include "wivrn_client.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <shared_mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>
#include <spdlog/spdlog.h>
using namespace std::chrono_literals;
using namespace wivrn;
static int checks=0, failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::printf("FAIL:%d %s\n",__LINE__,#x);}}while(false)
namespace wivrn {
namespace application { struct config { bool shard_retransmit=true; }; inline config current{}; inline config &get_config(){return current;} }
namespace scenes { struct stream { std::function<void(const from_headset::nack&)> send; void send_nack(const from_headset::nack& n){send(n);} }; }
class shard_accumulator {
public: using shard_set=wivrn::shard_set; 	using window_t = wivrn::frame_window<shard_set, 6, 3>;
 bool nxastc_codec=true; std::weak_ptr<scenes::stream> weak_scene; window_t window{shard_set{2}};
 std::vector<uint16_t> nack_scratch; uint64_t nack_requests=0,nack_shards=0; std::atomic<uint64_t> nack_shards_total=0;
 std::optional<XrTime> next_nack_deadline(XrTime); void poll_nacks(XrTime); void try_nack(XrTime); void report_nacks(XrTime){}
};
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
class poll_projection {
public:
 using control_socket_t=typed_socket<TCP,to_headset::packets,from_headset::packets>;
 using stream_socket_t=typed_socket<UDP,to_headset::packets,from_headset::packets>;
 stream_socket_t stream;control_socket_t control;control_socket_t secondary{-1};
 std::atomic<uint64_t> bytes_received_{0};std::shared_mutex secondary_mutex;uint64_t secondary_generation=0;std::atomic<bool> secondary_up{false};
 struct selector_type{bool control_up()const{return true;}} selector;
 poll_projection(stream_socket_t&&s,control_socket_t&&c):stream(std::move(s)),control(std::move(c)){}
 void poll_secondary_pending(auto&&){} void on_primary_received(bool){} bool on_control_send_error(const std::exception&){return false;} void on_stream_send_error(const std::exception&){} void drop_secondary(std::string_view){secondary_up=false;} void update_paths(){}
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


using namespace wivrn;
using data_shard=to_headset::video_stream_data_shard;
using data_tx=typed_socket<UDP,from_headset::packets,to_headset::packets>;
using ctl_tx=typed_socket<TCP,from_headset::packets,to_headset::packets>;
struct sockets {poll_projection::stream_socket_t rx;data_tx tx;poll_projection::control_socket_t crx;ctl_tx ctx;};
static sockets make_sockets(){UDP rx,tx; sockaddr_in6 a{};a.sin6_family=AF_INET6;a.sin6_addr=in6addr_loopback;a.sin6_port=0;rx.bind(a);tx.bind(a);sockaddr_in6 actual{},other{};socklen_t n=sizeof(actual);getsockname(rx.get_fd(),(sockaddr*)&actual,&n);n=sizeof(other);getsockname(tx.get_fd(),(sockaddr*)&other,&n);rx.connect(in6addr_loopback,ntohs(other.sin6_port));tx.connect(in6addr_loopback,ntohs(actual.sin6_port));TCPListener l(0);sockaddr_in6 ca{};n=sizeof(ca);getsockname(l.get_fd(),(sockaddr*)&ca,&n);TCP ctx(in6addr_loopback,ntohs(ca.sin6_port));auto [crx,peer]=l.accept<TCP>();return{poll_projection::stream_socket_t(std::move(rx)),data_tx(std::move(tx)),poll_projection::control_socket_t(std::move(crx)),ctl_tx(std::move(ctx))};}
static int64_t ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static std::array<std::array<std::array<uint8_t,16>,4>,100> payloads{};
static data_shard make_shard(uint64_t f,uint16_t i,bool end=false){data_shard d{};d.stream_item_idx=2;d.frame_idx=f;d.shard_idx=i;auto &bytes=payloads[f%100][i%4];bytes.fill(uint8_t(i+10));d.payload=bytes;if(i==0)d.view_info=data_shard::view_info_t{.display_time=42};if(end)d.timing_info=data_shard::timing_info_t{1,2,3,4};return d;}
static std::vector<uint16_t> requested(const from_headset::nack& n){std::vector<uint16_t> v;for(size_t b=0;b<n.bitmap.size();++b)for(int j=0;j<8;++j)if(n.bitmap[b]&(1u<<j))v.push_back(uint16_t(n.first_shard_idx+b*8+j));return v;}
int main(){
 std::puts("event,mono_ns,frame,index,count");auto sock=make_sockets();poll_projection session(std::move(sock.rx),std::move(sock.crx));shard_accumulator acc;auto scene=std::make_shared<scenes::stream>();scene->send=[&](auto&n){session.stream.send(from_headset::nack(n));std::printf("nack_send,%lld,40,%u,%zu\n",(long long)ns(),unsigned(n.first_shard_idx),requested(n).size());};acc.weak_scene=scene;
 bool completion_logged=false;auto visitor=[&](auto&& p){using P=std::decay_t<decltype(p)>;if constexpr(std::is_same_v<P,data_shard>){auto f=p.frame_idx;auto i=p.shard_idx;auto *q=acc.window.slot(f,[](shard_set&){});q->insert(std::move(p),ns());std::printf("client_shard,%lld,%llu,%u,0\n",(long long)ns(),(unsigned long long)f,unsigned(i));if(f==40&&q->complete()&&!completion_logged){completion_logged=true;std::printf("window_complete,%lld,40,3,1\n",(long long)ns());}}};
 // Sender's real retained-history format and decoder create the wire reply.
 shard_history hist;hist.set_enabled(true);std::array<data_shard,4> frame{make_shard(40,0),make_shard(40,1),make_shard(40,2),make_shard(40,3,true)};
 for(auto&d:frame){std::vector<uint8_t> blob;fec::encode_blob(d,blob);hist.push(40,d.shard_idx,blob,true);}hist.end_frame(40,4);
 sock.tx.send(data_shard(frame[0]));sock.tx.send(data_shard(frame[3]));std::this_thread::sleep_for(3ms);
 for(int i=0;i<3;++i){session.poll(visitor,10ms,[&]{return 2ms;});auto *q=acc.window.slot(40,[](shard_set&){});if(q->shards().size()>=4&&q->shards()[0]&&q->shards()[3])break;}auto &set=*acc.window.slot(40,[](shard_set&){});CHECK(set.shards().size()>=4&&set.shards()[0]&&set.shards()[3]&&!set.complete());if(set.shards().size()<4||!set.shards()[0]||!set.shards()[3])return 2;std::vector<uint16_t> holes;set.missing_shards(holes,false);std::printf("initial_hole,%lld,40,0,%zu\n",(long long)ns(),holes.size());CHECK(holes.size()==2&&holes[0]==1&&holes[1]==2);CHECK(set.feedback.stream_index==2);CHECK(set.shards()[0]->view_info.has_value()&&set.shards()[3]->timing_info.has_value());
 auto t=ns();auto deadline=acc.next_nack_deadline(t);CHECK(deadline.has_value());int pollret=session.poll(visitor,100ms,[&]{auto n=ns();return nack_poll_timeout(n,acc.next_nack_deadline(n),100ms);});std::printf("poll_return,%lld,40,0,%d\n",(long long)ns(),pollret);acc.poll_nacks(ns());
 pollfd pf{.fd=sock.tx.get_fd(),.events=POLLIN};CHECK(::poll(&pf,1,100)==1);auto packet=sock.tx.receive();CHECK(packet.has_value());auto nack=std::get<from_headset::nack>(std::move(*packet));auto want=requested(nack);CHECK(nack.stream_index==2&&nack.frame_idx==40&&want.size()==2&&want[0]==1&&want[1]==2);std::printf("peer_nack,%lld,40,%u,%zu\n",(long long)ns(),unsigned(nack.first_shard_idx),want.size());
 std::vector<shard_history::hit> hits;auto count=hist.collect(nack.frame_idx,nack.first_shard_idx,nack.bitmap,64,hits);CHECK(count==2&&hits.size()==2);for(auto&h:hits){auto d=fec::decode_blob(2,40,h.shard_idx,h.blob);CHECK(std::equal(d.payload.begin(),d.payload.end(),frame[h.shard_idx].payload.begin()));sock.tx.send(std::move(d));}std::printf("peer_repair_sent,%lld,40,1,%zu\n",(long long)ns(),hits.size());
 for(int i=0;i<3&&!set.complete();++i)session.poll(visitor,10ms,[&]{return 2ms;});CHECK(set.complete()&&completion_logged);CHECK(set.shards()[0]->view_info.has_value()&&set.shards()[3]->timing_info.has_value());for(uint16_t i=1;i<=2;++i)CHECK(set.shards()[i]->payload.size()==frame[i].payload.size()&&std::equal(set.shards()[i]->payload.begin(),set.shards()[i]->payload.end(),frame[i].payload.begin()));
 // Parity arrives and reconstructs the only interior hole before the due NACK check.
 shard_accumulator parity_acc;auto ps=std::make_shared<scenes::stream>();int parity_nacks=0;ps->send=[&](auto&){++parity_nacks;};parity_acc.weak_scene=ps;auto p0=make_shard(45,0),p1=make_shard(45,1),p2=make_shard(45,2,true);fec::group_builder gb;gb.set_layout(2,1);gb.reset(2,45);gb.add(p0);gb.add(p1);auto pb=gb.take();CHECK(pb.has_value());auto &p_set=*parity_acc.window.slot(45,[](shard_set&){});p_set.insert(data_shard(p0),3000);p_set.insert(data_shard(p2),3001);p_set.parity.push_back(*pb);auto fixed=p_set.reconstruct(p_set.parity[0],3002);CHECK(fixed&&*fixed==1&&p_set.complete());p_set.parity.clear();parity_acc.try_nack(3001+nack_quiet_period_ns);CHECK(parity_nacks==0);std::printf("parity_suppresses_nack,%lld,45,0,0\n",(long long)ns());
 // Two rounds and unknown newest tail use the exact accumulator methods without the socket seam.
 auto direct_scene=std::make_shared<scenes::stream>();int direct_nacks=0;direct_scene->send=[&](auto&){++direct_nacks;};shard_accumulator two;two.weak_scene=direct_scene;auto add=[&](uint64_t f,uint16_t i,int64_t at,bool end=false){auto d=make_shard(f,i,end);two.window.slot(f,[](shard_set&){})->insert(std::move(d),at);};add(50,0,1000);add(50,3,1001,true);add(51,0,1002);two.try_nack(1001+nack_quiet_period_ns);two.try_nack(1001+2*nack_quiet_period_ns);two.try_nack(1001+3*nack_quiet_period_ns);CHECK(direct_nacks==2&&two.window.slot(50,[](shard_set&){})->nack_rounds==2);std::printf("round_limit,%lld,50,2,%d\n",(long long)ns(),direct_nacks);
 shard_accumulator unknown;auto us=std::make_shared<scenes::stream>();int unknown_nacks=0;us->send=[&](auto&){++unknown_nacks;};unknown.weak_scene=us;auto u0=make_shard(60,0);auto u1=make_shard(60,1);unknown.window.slot(60,[](shard_set&){})->insert(std::move(u0),2000);unknown.window.slot(60,[](shard_set&){})->insert(std::move(u1),2001);unknown.try_nack(2001+nack_quiet_period_ns);CHECK(unknown_nacks==0);std::printf("newest_unknown_tail,%lld,60,0,0\n",(long long)ns());
 CHECK(acc.nack_requests==1&&acc.nack_shards==2);std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
