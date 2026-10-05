// Bitrate control law v2: the delivered-bandwidth estimator, driven on a virtual clock
// against a simulated link.
//
// The link model is the whole point of this harness. Given a capacity C and the pacing
// window w, one frame of B bits/s at F frames per second puts B/F bits on the wire and the
// headset sees them arrive over
//
//     wire = max(w * period, bytes * 8 / C)
//
// so the delivery rate the server can measure from the feedback is min(C, B / w): the link
// when the link is the bottleneck, the paced sending rate when it is not. Feeding that back
// through the control law has a single fixed point at 0.85 * C (or the ceiling, whichever
// binds), and every part below is a statement about how it gets there and what knocks it off.
//
// Part A: startup holds the ceiling while the estimate is still being learnt, and a link
//         with room to spare keeps it there.
// Part B: a link narrower than the ceiling is found and the bitrate settles at 0.85 of it.
// Part C: capacity that comes back is rediscovered by the periodic probe.
// Part D: loss backs off, and the link recovers afterwards.
// Part E: app-limited frames — small ones, delivered in a single micro-burst — never inflate
//         the estimate, however absurd the rate they imply.
// Part F: the utilisation floor that packet pacing puts under every sample does not read as
//         congestion, and does not stop the estimator converging.
// Part G: a degrading radio lowers the gain and blocks probing; a recovering one, or one that
//         simply stops falling and settles lower, releases it.
// Part H: the ceilings (client, path) and the floor clamp the estimate.
// Part I: switching control law mid-session starts over cleanly, both directions.
//
// Build:
//   g++ -std=c++23 -I server -I common -I build-server/common \
//       -I build-server/_deps/monado-src/src/xrt/include \
//       -I build-server/_deps/monado-src/src/xrt/auxiliary \
//       -I build-server/_deps/monado-src/src/external/openxr_includes \
//       -isystem external -isystem build-server/_deps/boost-src/libs/pfr/include \
//       -o bitrate_bbr_test tests/bitrate_bbr_test.cpp \
//       server/driver/bitrate_controller.cpp common/smp.cpp -lcrypto
//   ./bitrate_bbr_test

#include "driver/bitrate_controller.h"
#include "util/u_logging.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using wivrn::bitrate_controller;
using mode = bitrate_controller::mode;
using tp = bitrate_controller::clock::time_point;
using namespace std::chrono_literals;

namespace
{
int failures = 0;
int checks = 0;
bool verbose = false;

#define CHECK(...)                                                                           \
	do                                                                                   \
	{                                                                                    \
		++checks;                                                                    \
		if (not(__VA_ARGS__))                                                        \
		{                                                                            \
			++failures;                                                          \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
		}                                                                            \
	} while (0)

// 90 fps
constexpr int64_t period = 11'111'111;
constexpr uint32_t ceiling = 50'000'000;
constexpr float paced = 0.4f;

double mbit(uint64_t bps)
{
	return double(bps) * 1e-6;
}

// One controller against one simulated link, on a virtual clock.
struct harness
{
	bitrate_controller ctl;
	tp now;
	uint64_t frame = 0;

	// Link capacity in bits per second, 0 for "wider than anything asked of it"
	double capacity = 0;
	// Fraction of a frame period the shards are spread over, 0 when not paced
	float pacing = paced;

	std::vector<std::pair<tp, uint32_t>> changes;
	std::vector<std::pair<tp, uint32_t>> radio_changes;

	harness(mode control = mode::bbr,
	        double capacity_bps = 0,
	        float pacing_window = paced,
	        bool enabled = true,
	        bool client_enabled = true,
	        bool radio_aware = true) :
	        capacity(capacity_bps), pacing(pacing_window)
	{
		// Not the epoch: an unset "last decrease" is the epoch, and starting there
		// would put every cooldown inside its own window.
		now = tp{} + 1h;
		ctl.configure({.enabled = enabled}, ceiling, client_enabled, radio_aware, control);
		ctl.set_pacing_window(pacing_window);
	}

	void advance(std::chrono::nanoseconds d)
	{
		now += d;
	}

	uint32_t current() const
	{
		return ctl.current();
	}

	// Bytes one frame of video is worth at the bitrate currently in force.
	uint64_t frame_bytes() const
	{
		return uint64_t(double(current()) * double(period) / 8e9);
	}

	// How long the headset spends receiving that frame on the simulated link.
	int64_t wire_ns(uint64_t bytes) const
	{
		int64_t transmission = capacity > 0
		                               ? int64_t(8e9 * double(bytes) / capacity)
		                               : int64_t(0.05 * double(period));
		if (pacing > 0)
			transmission = std::max(transmission, int64_t(double(pacing) * double(period)));
		return transmission;
	}

	// One frame on stream 0, with an explicit byte count and wire time. Everything else
	// derives from these two.
	void feed_raw(uint64_t bytes, int64_t wire, bool lost = false, bool late = false)
	{
		const uint64_t index = frame++;

		if (bytes)
			ctl.on_frame_bytes(index, 0, uint32_t(bytes), now);

		wivrn::from_headset::feedback fb{};
		fb.frame_index = index;
		fb.stream_index = 0;

		const XrTime base = 1'000'000'000 + XrTime(index) * period;
		fb.received_first_packet = base;

		if (not lost)
		{
			fb.received_last_packet = base + wire;
			fb.sent_to_decoder = base + wire;
			fb.received_from_decoder = base + wire + period;
			if (not late)
			{
				fb.blitted = base + wire + 2 * period;
				fb.times_displayed = 1;
			}
		}

		if (auto b = ctl.on_feedback(fb, period, true, now))
			changes.emplace_back(now, *b);

		advance(std::chrono::nanoseconds(period));
	}

	// One frame the way the simulated link would deliver it.
	void feed_frame(bool lost = false, bool late = false)
	{
		const uint64_t bytes = frame_bytes();
		feed_raw(bytes, wire_ns(bytes), lost, late);
	}

	void feed(size_t n)
	{
		for (size_t i = 0; i < n; ++i)
			feed_frame();
	}

	// n seconds of video at the display rate.
	void seconds(double n)
	{
		feed(size_t(n * 90));
	}

	// Feed until the bitrate has been still for two seconds, so that an assertion never
	// lands in the middle of the periodic probe or of the drain right after it. Returns
	// the bitrate it came to rest at.
	uint32_t quiet(double max_seconds = 60)
	{
		uint32_t last = current();
		int still = 0;
		for (int i = 0; i < int(max_seconds * 2); ++i)
		{
			feed(45);
			if (current() == last)
			{
				if (++still >= 4)
					return last;
			}
			else
			{
				last = current();
				still = 0;
			}
		}
		return current();
	}

	void feed_radio(int rssi, int link_speed = 866)
	{
		if (auto b = ctl.on_wifi_state(rssi, link_speed, now))
		{
			changes.emplace_back(now, *b);
			radio_changes.emplace_back(now, *b);
		}
	}

	// One second of video with one radio report at the start of it.
	void second(int rssi, int link_speed = 866)
	{
		feed_radio(rssi, link_speed);
		feed(90);
	}
};

struct rate_stream
{
	uint32_t bytes;
	int64_t offset_ns;
	int64_t span_ns;
	bool lost = false;
	bool missing_timing = false;
	int64_t send_offset_ns = 0;
	int64_t send_span_ns = 0;
	bool send_valid = false;
};

std::pair<double, uint32_t> stereo_rate(const std::vector<rate_stream> & streams, bool duplicate_first_feedback = false)
{
	bitrate_controller ctl;
	auto now = tp{} + 1h;
	ctl.configure({.enabled = true}, 1'000'000'000, true, false, mode::bbr);
	ctl.set_pacing_window(paced);

	for (uint64_t index = 0; index < 60; ++index)
	{
		const XrTime base = 10'000'000'000LL + XrTime(index) * 20'000'000;
		for (uint8_t stream = 0; stream < streams.size(); ++stream)
		{
			const auto & input = streams[stream];
			ctl.on_frame_bytes(index, stream, input.bytes, now);
			wivrn::from_headset::feedback fb{};
			fb.frame_index = index;
			fb.stream_index = stream;
			if (not input.lost)
			{
				if (not input.missing_timing)
				{
					fb.received_first_packet = base + input.offset_ns;
					fb.received_last_packet = fb.received_first_packet + input.span_ns;
					fb.sent_to_decoder = std::max(fb.received_first_packet, fb.received_last_packet) + 1'000'000;
					if (input.send_valid)
					{
						fb.send_begin = base + input.send_offset_ns;
						fb.send_end = fb.send_begin + input.send_span_ns;
					}
				}
				else
					fb.sent_to_decoder = base + 20'000'000; // Complete, but no usable receive interval.
			}
			ctl.on_feedback(fb, period, true, now);
			if (duplicate_first_feedback and index == 20 and stream == 0)
				ctl.on_feedback(fb, period, true, now);
		}
		now += 20ms;
	}
	return {double(ctl.bandwidth_estimate()), ctl.current()};
}

std::pair<double, uint32_t> stereo_send_rate(const std::vector<rate_stream> & streams,
                                                int64_t send_clock_shift = 0,
                                                bool split_duplicate_send = false,
                                                bool split_malformed_send = false,
                                                bool direct_quality = false)
{
	bitrate_controller ctl;
	auto now = tp{} + 1h;
	ctl.configure({.enabled = true}, 1'000'000'000, true, false, mode::bbr);
	ctl.set_pacing_window(paced);
	for (uint64_t index = 0; index < 60; ++index)
	{
		const XrTime base = 10'000'000'000LL + XrTime(index) * 20'000'000;
		for (uint8_t stream = 0; stream < streams.size(); ++stream)
		{
			const auto & input = streams[stream];
			ctl.on_frame_bytes(index, stream, input.bytes, now,
			                   direct_quality ? 50'000'000 : 0,
			                   direct_quality ? period : 0);
			wivrn::from_headset::feedback fb{};
			fb.frame_index = index;
			fb.stream_index = stream;
			fb.received_first_packet = base + input.offset_ns;
			fb.received_last_packet = fb.received_first_packet + input.span_ns;
			fb.sent_to_decoder = fb.received_last_packet + 1'000'000;
			if (input.send_valid)
			{
				fb.send_begin = 900'000'000'000LL + send_clock_shift + input.send_offset_ns;
				fb.send_end = fb.send_begin + input.send_span_ns;
			}
			if (split_duplicate_send)
			{
				const XrTime full_end = fb.send_end;
				fb.send_end = fb.send_begin + input.send_span_ns / 2;
				ctl.on_feedback(fb, period, true, now);
				fb.send_begin += input.send_span_ns / 2;
				fb.send_end = full_end;
			}
			else if (split_malformed_send)
			{
				const XrTime full_end = fb.send_end;
				fb.send_end = 0; // Invalid begin-only observation.
				ctl.on_feedback(fb, period, true, now);
				fb.send_begin = 0;
				fb.send_end = full_end; // Invalid end-only observation.
			}
			ctl.on_feedback(fb, period, true, now);
		}
		now += 20ms;
	}
	return {double(ctl.bandwidth_estimate()), ctl.current()};
}

void matched_send_spans_bound_rate_without_clock_origin_comparison()
{
	const std::vector receive_fast_send_overlap{
	        rate_stream{200'000, 0, 6'000'000, false, false, 0, 6'000'000, true},
	        rate_stream{200'000, 0, 6'000'000, false, false, 0, 6'000'000, true}};
	const std::vector receive_fast_send_serial{
	        rate_stream{200'000, 0, 6'000'000, false, false, 0, 6'000'000, true},
	        rate_stream{200'000, 0, 6'000'000, false, false, 6'000'000, 6'000'000, true}};
	const auto overlap = stereo_send_rate(receive_fast_send_overlap);
	const auto serial = stereo_send_rate(receive_fast_send_serial);
	const auto shifted = stereo_send_rate(receive_fast_send_serial, 7'000'000'000LL);
	const auto merged_duplicates = stereo_send_rate(receive_fast_send_serial, 0, true);
	const auto malformed_duplicates = stereo_send_rate(receive_fast_send_serial, 0, false, true);
	const std::vector direct{rate_stream{400'000, 0, 6'000'000, false, false, 0, 12'000'000, true}};
	const auto direct_quality_rate = stereo_send_rate(direct, 0, false, false, true);
	CHECK(std::abs(overlap.first - 533'333'333.0) < 1'000);
	CHECK(std::abs(serial.first - 266'666'667.0) < 1'000);
	CHECK(std::abs(shifted.first - serial.first) < 1'000);
	CHECK(std::abs(merged_duplicates.first - serial.first) < 1'000);
	CHECK(std::abs(malformed_duplicates.first - 533'333'333.0) < 1'000);
	CHECK(std::abs(direct_quality_rate.first - 533'333'333.0) < 1'000);

	// One missing, zero, or reversed byte-contributing send interval falls back to D's
	// receive-only denominator for the whole sample.
	for (int invalid = 0; invalid != 3; ++invalid)
	{
		auto streams = receive_fast_send_serial;
		streams[1].send_valid = invalid != 0;
		if (invalid == 1)
			streams[1].send_span_ns = 0;
		if (invalid == 2)
		{
			streams[1].send_offset_ns = 12'000'000;
			streams[1].send_span_ns = -1;
		}
		const auto fallback = stereo_send_rate(streams);
		CHECK(std::abs(fallback.first - 533'333'333.0) < 1'000);
	}
	std::printf("  matched send spans (Mbps): overlap=%.3f serial=%.3f shifted=%.3f duplicate-merged=%.3f malformed-fallback=%.3f direct-quality=%.3f\n",
	            overlap.first / 1e6, serial.first / 1e6, shifted.first / 1e6,
	            merged_duplicates.first / 1e6, malformed_duplicates.first / 1e6, direct_quality_rate.first / 1e6);
}

void stereo_rate_uses_matching_stream_bytes_and_union_span()
{
	const std::vector one{rate_stream{400'000, 0, 6'000'000}};
	const std::vector overlap{rate_stream{200'000, 0, 6'000'000}, rate_stream{200'000, 0, 6'000'000}};
	const std::vector sequential{rate_stream{200'000, 0, 6'000'000}, rate_stream{200'000, 6'000'000, 6'000'000}};
	const std::vector partial{rate_stream{200'000, 0, 6'000'000}, rate_stream{200'000, 3'000'000, 6'000'000}};
	const std::vector gap{rate_stream{200'000, 0, 6'000'000}, rate_stream{200'000, 12'000'000, 6'000'000}};
	const std::vector unequal{rate_stream{100'000, 0, 3'000'000}, rate_stream{300'000, 1'500'000, 6'000'000}};
	const std::vector untimed{rate_stream{200'000, 0, 6'000'000}, rate_stream{200'000, 0, 0, false, true}};
	const std::vector zero_span{rate_stream{200'000, 0, 6'000'000}, rate_stream{200'000, 0, 0}};
	const std::vector reversed{rate_stream{200'000, 0, 6'000'000}, rate_stream{200'000, 12'000'000, -1'000'000}};
	const std::vector chain{rate_stream{100'000, 0, 3'000'000}, rate_stream{100'000, 2'000'000, 3'000'000}, rate_stream{100'000, 4'000'000, 2'000'000}};
	const std::vector short_bytes_long_timing{rate_stream{200'000, 0, 500'000}, rate_stream{0, 0, 6'000'000}};
	const std::vector no_timing{rate_stream{200'000, 0, 0, false, true}, rate_stream{200'000, 0, 0, false, true}};

	const auto [one_rate, one_target] = stereo_rate(one);
	const auto [overlap_rate, overlap_target] = stereo_rate(overlap);
	const auto [serial_rate, serial_target] = stereo_rate(sequential, true);
	const auto [partial_rate, _partial_target] = stereo_rate(partial);
	const auto [gap_rate, _gap_target] = stereo_rate(gap);
	const auto [unequal_rate, _unequal_target] = stereo_rate(unequal);
	const auto [untimed_rate, _untimed_target] = stereo_rate(untimed);
	const auto [zero_rate, _zero_target] = stereo_rate(zero_span);
	const auto [reversed_rate, _reversed_target] = stereo_rate(reversed);
	const auto [chain_rate, _chain_target] = stereo_rate(chain);
	const auto [short_rate, _short_target] = stereo_rate(short_bytes_long_timing);
	const auto [no_timing_rate, _no_timing_target] = stereo_rate(no_timing);
	std::printf("  matched receive-span samples (Mbps): one=%.3f/%u overlap=%.3f/%u serial=%.3f/%u partial=%.3f gap=%.3f unequal=%.3f untimed=%.3f no-timing=%.3f\n",
	            one_rate / 1e6, one_target, overlap_rate / 1e6, overlap_target, serial_rate / 1e6, serial_target,
	            partial_rate / 1e6, gap_rate / 1e6, unequal_rate / 1e6, untimed_rate / 1e6, no_timing_rate / 1e6);

	CHECK(std::abs(one_rate - 533'333'333.0) < 1'000);
	CHECK(std::abs(overlap_rate - one_rate) < 1'000);
	CHECK(std::abs(serial_rate - 266'666'667.0) < 1'000);
	CHECK(serial_target < overlap_target);
	CHECK(std::abs(partial_rate - 355'555'556.0) < 1'000);
	CHECK(std::abs(gap_rate - 266'666'667.0) < 1'000); // The idle gap is not counted.
	CHECK(std::abs(unequal_rate - 426'666'667.0) < 1'000);
	CHECK(std::abs(untimed_rate - 266'666'667.0) < 1'000);
	CHECK(std::abs(zero_rate - 266'666'667.0) < 1'000);
	CHECK(std::abs(reversed_rate - 266'666'667.0) < 1'000);
	CHECK(std::abs(chain_rate - 400'000'000.0) < 1'000);
	CHECK(short_rate == no_timing_rate); // A zero-byte stream's long span cannot admit the short burst.
	CHECK(no_timing_rate == 0);
}

// The value the control law converges to on a link of capacity C, before any ceiling.
uint32_t settled(double capacity)
{
	return uint32_t(bitrate_controller::gain_steady * capacity);
}

bool near(uint32_t value, double expected, double tolerance = 0.08)
{
	return std::abs(double(value) - expected) <= tolerance * expected;
}

void part_a()
{
	std::printf("Part A: a link with room to spare stays at the ceiling\n");

	// Ten times the ceiling: whatever the bitrate, the link swallows it and the only thing
	// limiting the measured rate is the pacing window.
	{
		harness h(mode::bbr, 500e6);
		h.seconds(20);

		CHECK(h.current() == ceiling);
		CHECK(h.changes.empty());
	}

	// The same, unpaced. Every frame is over long before it loaded the link, so there is
	// no delivery rate sample at all and the controller has nothing to lower the bitrate
	// with — which is exactly right, nothing is wrong.
	{
		harness h(mode::bbr, 500e6, /* pacing = */ 0);
		h.seconds(20);

		CHECK(h.current() == ceiling);
		CHECK(h.changes.empty());
	}
}

void part_b()
{
	std::printf("Part B: a narrow link is measured and settled into\n");

	// 24 Mbit/s against a 50 Mbit/s ceiling. The first frames overrun a frame period, so
	// the acute path fires once, and from there the estimate takes over.
	{
		const double capacity = 24e6;
		harness h(mode::bbr, capacity);
		const uint32_t at = h.quiet();

		CHECK(not h.changes.empty());
		CHECK(near(at, settled(capacity)));
		if (verbose)
			std::printf("    settled at %.1f Mbit/s, expected %.1f (%zu changes)\n",
			            mbit(at),
			            mbit(settled(capacity)),
			            h.changes.size());

		// And it stays there. The only thing that still moves it is the periodic probe,
		// which raises the bitrate for one round and drains straight back: two changes
		// per probe interval, and it always comes back to the same number.
		const size_t before = h.changes.size();
		h.seconds(20);
		CHECK(h.changes.size() - before <= 2 * (20 / 8 + 1));
		// The same number, up to the last bit: the byte counts are integers and which
		// sample won the maximum filter moves the estimate by parts per million.
		CHECK(near(h.quiet(), at, 0.001));
	}

	// Same link, no pacing. The delivery rate is then a direct measurement of the link on
	// every frame, and the same fixed point must come out.
	{
		const double capacity = 24e6;
		harness h(mode::bbr, capacity, /* pacing = */ 0);

		CHECK(near(h.quiet(), settled(capacity)));
	}

	// A link narrow enough that the floor binds instead.
	{
		harness h(mode::bbr, 4e6);

		CHECK(h.quiet() == bitrate_controller::config{}.min_bitrate_bps);
	}
}

void part_c()
{
	std::printf("Part C: capacity that comes back is rediscovered\n");

	harness h(mode::bbr, 20e6);
	const uint32_t narrow = h.quiet();
	CHECK(near(narrow, settled(20e6)));

	// The user walks back towards the access point.
	h.capacity = 45e6;
	const uint32_t wide = h.quiet();

	CHECK(wide > narrow);
	CHECK(near(wide, settled(45e6)));
	if (verbose)
		std::printf("    %.1f -> %.1f Mbit/s after the link widened\n", mbit(narrow), mbit(wide));

	// And all the way back to a link wider than the ceiling.
	h.capacity = 500e6;
	CHECK(h.quiet() == ceiling);
}

void part_d()
{
	std::printf("Part D: loss backs off, and the link recovers afterwards\n");

	harness h(mode::bbr, 40e6);
	const uint32_t settled_at = h.quiet();
	CHECK(near(settled_at, settled(40e6)));

	// An acute spike with nothing wrong with the capacity itself: frames that never
	// arrived at all.
	const size_t before = h.changes.size();
	for (int i = 0; i < 180; ++i)
		h.feed_frame(/* lost = */ i % 6 == 0);

	CHECK(h.changes.size() > before);
	CHECK(h.current() < settled_at);
	if (verbose)
		std::printf("    %.1f -> %.1f Mbit/s on loss\n", mbit(settled_at), mbit(h.current()));

	// The loss stops. The estimate is still the same link, so the bitrate comes back.
	CHECK(near(h.quiet(), settled(40e6)));
}

void loss_with_bursty_delivery()
{
	std::printf("Part D2: loss cannot turn bursty delivery into an upward backoff\n");
	harness h(mode::bbr, 20e6);
	h.quiet();
	const size_t first_change = h.changes.size();
	uint32_t previous = h.current();
	// Good packets arrive in a short burst while half the frames are lost.
	// Their inflated delivery estimate must not raise the rate during backoff.
	for (int i = 0; i < 360; ++i)
	{
		h.feed_raw(h.frame_bytes(), period / 2, i % 2 == 0);
		if (h.current() != previous)
		{
			CHECK(h.current() <= previous);
			previous = h.current();
		}
	}
	CHECK(h.changes.size() > first_change);
}

void part_e()
{
	std::printf("Part E: app-limited frames are not capacity measurements\n");

	// Settle on a narrow link first, so there is an estimate to inflate.
	harness h(mode::bbr, 20e6);
	const uint32_t settled_at = h.quiet();
	CHECK(near(settled_at, settled(20e6)));

	// Now a long stretch of nearly static scene: 2 kB frames that go out in one
	// micro-burst and land in 200 us. 2 kB in 200 us is 80 Mbit/s of "capacity", four
	// times the link, and it must not move the estimate one bit.
	for (int i = 0; i < 900; ++i)
		h.feed_raw(2048, 200'000);

	CHECK(h.current() == settled_at);

	// The absurd end of the same idea: one byte delivered in one nanosecond.
	for (int i = 0; i < 90; ++i)
		h.feed_raw(1, 1);

	CHECK(h.current() == settled_at);

	// And once real frames come back, the real link is still what is measured.
	CHECK(near(h.quiet(), settled(20e6)));
}

void part_f()
{
	std::printf("Part F: the paced utilisation floor is not congestion\n");

	// With pacing on, every frame occupies at least 40% of a frame period whatever its
	// size. v1 reads that as a utilisation of 0.4, which sits below its 0.60 probe-up
	// threshold by design. v2 must not read it as anything at all.
	{
		harness h(mode::bbr, 200e6);
		h.seconds(30);

		CHECK(h.current() == ceiling);
		CHECK(h.changes.empty());
	}

	// A wider pacing window, right at the clamp, still floors nothing that matters.
	{
		harness h(mode::bbr, 200e6, /* pacing = */ 0.5f);
		h.seconds(30);

		CHECK(h.current() == ceiling);
	}

	// And the convergence on a narrow link is the same whatever the window: the estimate
	// divides the bytes out, so the floor cancels.
	{
		harness a(mode::bbr, 24e6, 0.2f);
		harness b(mode::bbr, 24e6, 0.5f);

		CHECK(near(a.quiet(), settled(24e6)));
		CHECK(near(b.quiet(), settled(24e6)));
	}
}

void part_g()
{
	std::printf("Part G: a degrading radio lowers the gain and blocks probing\n");

	harness h(mode::bbr, 30e6);

	// Settle, with a flat signal on the record.
	for (int i = 0; i < 15; ++i)
		h.second(-52);
	const uint32_t settled_at = h.current();
	CHECK(near(settled_at, settled(30e6)));

	// Walk away from the access point, at a normal walking pace.
	int rssi = -52;
	for (int i = 0; i < 15 and h.radio_changes.empty(); ++i)
	{
		rssi -= 2;
		h.second(rssi);
	}

	CHECK(h.radio_changes.size() == 1);
	// The radio gain, not the steady one, and never above where it already was.
	CHECK(h.current() < settled_at);
	CHECK(near(h.current(), bitrate_controller::gain_radio * 30e6));
	const uint32_t held = h.current();
	if (verbose)
		std::printf("    radio step at %d dBm: %.1f -> %.1f Mbit/s\n", rssi, mbit(settled_at), mbit(held));

	// While the signal is genuinely still falling the hold stays and the bitrate must not
	// walk back up: probing into a falling signal is the mistake the hold exists to
	// prevent. Kept inside the plausible RSSI range so the fall is unambiguous rather than
	// a signal that has quietly flattened out (which, correctly, would release the hold).
	for (int i = 0; i < 6; ++i)
	{
		rssi -= 2;
		h.second(rssi);
	}

	CHECK(h.current() <= held);

	// The signal recovers. The steady gain comes back, and with it the bitrate.
	for (int i = 0; i < 8; ++i)
	{
		rssi += 3;
		h.second(rssi);
	}
	for (int i = 0; i < 15; ++i)
		h.second(-50);

	CHECK(h.current() > held);
	CHECK(near(h.current(), settled(30e6)));
	// The radio never returned a bitrate on the way up.
	CHECK(h.radio_changes.size() == 1);

	// A signal that simply stops falling, without climbing back, releases the hold too:
	// the user walked to a new spot and settled at a lower but steady level, reports
	// still coming and the link healthy. Without the stabilisation release the bitrate
	// would stay latched below the ceiling forever.
	{
		harness h2(mode::bbr, 30e6);
		for (int i = 0; i < 15; ++i)
			h2.second(-52);
		const uint32_t settled2 = h2.current();

		int r = -52;
		for (int i = 0; i < 15 and h2.radio_changes.empty(); ++i)
		{
			r -= 2;
			h2.second(r);
		}
		CHECK(h2.radio_changes.size() == 1);
		const uint32_t held2 = h2.current();
		CHECK(held2 < settled2);

		// Flat from here: the fall has stopped, and the hold must not outlive it.
		for (int i = 0; i < 30; ++i)
			h2.second(r);

		CHECK(h2.current() > held2);
		CHECK(near(h2.current(), settled(30e6)));
		CHECK(h2.radio_changes.size() == 1);
	}
}

void part_h()
{
	std::printf("Part H: ceilings and the floor clamp the estimate\n");

	// A path ceiling below where the estimate would put the bitrate.
	{
		harness h(mode::bbr, 60e6);
		h.seconds(15);
		CHECK(h.current() == ceiling);

		h.ctl.set_path_ceiling(12'000'000);
		CHECK(h.current() == 12'000'000);

		h.seconds(20);
		CHECK(h.current() <= 12'000'000);

		// Back to the client's, which also starts the estimator over.
		h.ctl.set_path_ceiling(std::nullopt);
		CHECK(h.current() == ceiling);
		h.seconds(20);
		CHECK(h.current() == ceiling);
	}

	// The floor, against a link that cannot carry even that.
	{
		harness h(mode::bbr, 2e6);
		CHECK(h.quiet() == bitrate_controller::config{}.min_bitrate_bps);
	}

	// Both switches still gate the whole thing, v2 like v1.
	{
		harness h(mode::bbr, 8e6, paced, /* enabled = */ false);
		h.seconds(20);
		CHECK(h.current() == ceiling);
		CHECK(h.changes.empty());
	}
	{
		harness h(mode::bbr, 8e6, paced, true, /* client_enabled = */ false);
		h.seconds(20);
		CHECK(h.current() == ceiling);
		CHECK(h.changes.empty());
	}
}

void part_i()
{
	std::printf("Part I: switching control law mid-session\n");

	// v1 first, then v2. The AIMD converges somewhere of its own; the switch must put the
	// bitrate back at the ceiling and let the estimator start from nothing.
	{
		harness h(mode::aimd, 24e6);
		h.seconds(20);
		const uint32_t aimd_at = h.current();
		CHECK(aimd_at < ceiling);
		CHECK(h.ctl.active_mode() == mode::aimd);

		auto applied = h.ctl.set_client_mode(mode::bbr);
		CHECK(applied.has_value());
		CHECK(*applied == ceiling);
		CHECK(h.current() == ceiling);
		CHECK(h.ctl.active_mode() == mode::bbr);

		// And now the other law converges on its own terms, from scratch.
		CHECK(near(h.quiet(), settled(24e6)));
	}

	// v2 first, then v1.
	{
		harness h(mode::bbr, 24e6);
		CHECK(near(h.quiet(), settled(24e6)));

		auto applied = h.ctl.set_client_mode(mode::aimd);
		CHECK(applied.has_value());
		CHECK(*applied == ceiling);
		CHECK(h.ctl.active_mode() == mode::aimd);

		h.seconds(25);
		CHECK(h.current() < ceiling);
	}

	// The headset expressing no preference leaves the server configuration in charge, and
	// expressing one takes it back.
	{
		harness h(mode::aimd, 24e6);
		CHECK(h.ctl.active_mode() == mode::aimd);

		// Same as the server default: no change, nothing applied.
		CHECK(not h.ctl.set_client_mode(mode::aimd).has_value());
		CHECK(h.ctl.active_mode() == mode::aimd);

		CHECK(h.ctl.set_client_mode(mode::bbr).has_value());
		CHECK(h.ctl.active_mode() == mode::bbr);

		// Back to no preference: the configured default takes over again.
		CHECK(h.ctl.set_client_mode(std::nullopt).has_value());
		CHECK(h.ctl.active_mode() == mode::aimd);
	}

	// A server configured for v2, with a headset that never chose.
	{
		bitrate_controller ctl;
		ctl.configure({.enabled = true, .control = mode::bbr}, ceiling, true, true, std::nullopt);
		CHECK(ctl.active_mode() == mode::bbr);
	}
}
} // namespace

// The controller logs through Monado's u_log; standing in for it is all it takes to run the
// policy on its own, with no server, no encoder and no headset.
extern "C" void u_log(const char *, int, const char *, enum u_logging_level, const char * format, ...)
{
	if (not verbose)
		return;

	std::printf("    [log] ");
	va_list args;
	va_start(args, format);
	std::vprintf(format, args);
	va_end(args);
	std::printf("\n");
}

extern "C" enum u_logging_level u_log_get_global_level(void)
{
	return U_LOGGING_INFO;
}

// A rising capacity maximum must not be compared with the old rate cohort.
// Conversely, one loaded fast burst is not permission to double a steady target.
void fresh_peak_requires_matching_evidence()
{
	{
		harness h(mode::bbr, 24e6);
		h.quiet();
		const auto before = h.current();
		h.capacity = 48e6;
		uint32_t lowest = before;
		for (int i = 0; i < 270; ++i)
		{
			h.feed_frame();
			lowest = std::min(lowest, h.current());
		}
		CHECK(lowest >= before);
		CHECK(h.current() >= 40'000'000);
	}
	// Both phases previously let an unconfirmed post-peak p90 raise the target.
	for (int burst_frame: {8, 31})
	{
		harness h(mode::bbr, 24e6);
		h.quiet();
		uint32_t highest = h.current();
		for (int i = 0; i < 270; ++i)
		{
			const auto bytes = h.frame_bytes();
			const auto wire = i == burst_frame
			                        ? std::max<int64_t>(period * paced, int64_t(8e9 * double(bytes) / 48e6))
			                        : h.wire_ns(bytes);
			h.feed_raw(bytes, wire);
			highest = std::max(highest, h.current());
		}
		CHECK(highest <= 24'000'000);
	}
}


// A late fast burst must not inflate the periodic probe or its completion.
void probe_growth_uses_delivery_evidence()
{
	for (int burst_frame: {38, 46})
	{
		harness h(mode::bbr, 24e6);
		h.quiet();
		h.feed(360);
		uint32_t highest = h.current();
		for (int i = 0; i < 270; ++i)
		{
			const auto bytes = h.frame_bytes();
			const auto wire = i == burst_frame
			                        ? std::max<int64_t>(period * paced, int64_t(8e9 * double(bytes) / 48e6))
			                        : h.wire_ns(bytes);
			h.feed_raw(bytes, wire);
			highest = std::max(highest, h.current());
		}
		// The ordinary 1.1-gain probe peaks near26.4Mbps, not the50Mbps ceiling.
		CHECK(highest <= 26'500'000);
	}
}

int main(int argc, char ** argv)
{
	verbose = argc > 1 and std::string(argv[1]) == "-v";

	stereo_rate_uses_matching_stream_bytes_and_union_span();
	matched_send_spans_bound_rate_without_clock_origin_comparison();
	fresh_peak_requires_matching_evidence();
	probe_growth_uses_delivery_evidence();
	part_a();
	part_b();
	part_c();
	part_d();
	loss_with_bursty_delivery();
	part_e();
	part_f();
	part_g();
	part_h();
	part_i();

	std::printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
