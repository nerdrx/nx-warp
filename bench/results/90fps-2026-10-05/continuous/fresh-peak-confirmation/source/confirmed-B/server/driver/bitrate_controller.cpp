/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn NX contributors
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

#include "bitrate_controller.h"

#include "util/u_logging.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace wivrn
{

namespace
{
constexpr double to_mbits = 1e-6;

const char * mode_name(bitrate_controller::mode m)
{
	return m == bitrate_controller::mode::bbr ? "bandwidth estimation (v2)" : "AIMD (v1)";
}
} // namespace

void bitrate_controller::configure(const config & c, uint32_t ceiling_bps, bool client_enabled_, bool radio_aware_, std::optional<mode> client_mode_)
{
	std::lock_guard lock(mutex);

	conf = c;
	const char * loss_only_env = std::getenv("WIVRN_BITRATE_AIMD_LOSS_ONLY");
	aimd_loss_only = loss_only_env && std::strcmp(loss_only_env, "1") == 0;
	client_enabled = client_enabled_;
	radio_aware = radio_aware_;
	client_mode = client_mode_;
	ceiling = ceiling_bps;
	auto eff = effective_ceiling();
	// A client asking for less than the configured minimum wins: the ceiling is always honoured.
	min_bitrate = std::min(conf.min_bitrate_bps, eff ? eff : conf.min_bitrate_bps);
	bitrate = eff;
	recovery_target = eff;
	st = state::steady;
	first_recovery_step = true;
	aimd_probe_ratio = 0.35;
	quality_stream_mask = measured_stream_mask = 0;
	frames = {};
	has_frames = false;
	stale_before = 0;
	flush();
	flush_radio();
	flush_estimator();

	if (conf.enabled and client_enabled and eff)
		U_LOG_I("Automatic bitrate enabled, %s, ceiling %.1f Mbit/s, floor %.1f Mbit/s, radio trend %s",
		        mode_name(mode_locked()),
		        eff * to_mbits,
		        min_bitrate * to_mbits,
		        radio_aware ? "on" : "off");
	else if (conf.enabled and not client_enabled and eff)
		U_LOG_I("Automatic bitrate disabled on the headset, using %.1f Mbit/s", eff * to_mbits);
	if (aimd_loss_only and mode_locked() == mode::aimd)
		U_LOG_I("AIMD diagnostic: utilisation-only decreases disabled; loss/late protection remains enabled");
}

uint32_t bitrate_controller::effective_ceiling() const
{
	// While the two paths are combined the USB budget is not a ceiling on the session: it
	// caps a session running *on* the tunnel, and this one only spills the tail of every
	// frame onto it. The client's own ceiling still stands, and is the only one.
	if (path_ceiling and ceiling and not combined)
		return std::min(ceiling, *path_ceiling);
	return ceiling;
}

bool bitrate_controller::enabled() const
{
	std::lock_guard lock(mutex);
	return conf.enabled and client_enabled;
}

bitrate_controller::mode bitrate_controller::mode_locked() const
{
	// The headset's selector wins whenever it expressed one; the configuration key is the
	// default for a headset that never touched it.
	return client_mode.value_or(conf.control);
}

bitrate_controller::mode bitrate_controller::active_mode() const
{
	std::lock_guard lock(mutex);
	return mode_locked();
}

bitrate_controller::status bitrate_controller::snapshot() const
{
	using controller_state = to_headset::transport_status::controller_state;

	std::lock_guard lock(mutex);

	status s{
	        .bitrate_bps = bitrate,
	        .ceiling_bps = effective_ceiling(),
	        .control = mode_locked(),
	};

	if (not(conf.enabled and client_enabled))
	{
		// Nothing is controlling anything: the bitrate is the one the headset set, and
		// neither state machine has any meaning to report.
		s.state = controller_state::off;
		return s;
	}

	if (s.control == mode::bbr)
	{
		switch (bbr_st)
		{
			case bbr_state::startup:
				s.state = controller_state::startup;
				break;
			case bbr_state::steady:
				s.state = controller_state::steady;
				break;
			case bbr_state::probe:
				s.state = controller_state::probe;
				break;
		}
	}
	else
	{
		s.state = st == state::recovering ? controller_state::recovering : controller_state::steady;
	}

	// The hold is a latch that outlives the switch being turned off; it only holds anything
	// back while the switch is on, so that is what the page is told.
	s.radio_hold = radio_hold and radio_aware;

	s.emergency = emergency_active;

	return s;
}

std::optional<uint32_t> bitrate_controller::set_client_mode(std::optional<mode> m)
{
	std::lock_guard lock(mutex);

	if (m == client_mode)
		return {};

	const mode before = mode_locked();
	client_mode = m;
	const mode after = mode_locked();

	if (before == after)
		return {};

	U_LOG_I("Automatic bitrate: control law is now %s", mode_name(after));

	// The two laws do not measure the same thing: a utilisation window says nothing about a
	// bandwidth estimate and the other way round. Start over, from the ceiling, both ways.
	return reset_locked();
}

void bitrate_controller::set_pacing_window(float window)
{
	std::lock_guard lock(mutex);
	pacing_window = std::clamp(window, 0.f, 1.f);
}

std::optional<uint32_t> bitrate_controller::set_client_enabled(bool enabled_)
{
	std::lock_guard lock(mutex);

	if (enabled_ == client_enabled)
		return {};

	client_enabled = enabled_;

	U_LOG_I("Automatic bitrate %s on the headset", client_enabled ? "enabled" : "disabled");

	// Measurements taken under the other setting say nothing about it now. Starting over also
	// puts the bitrate back to the ceiling, which is what switching off must do.
	return reset_locked();
}

void bitrate_controller::set_radio_aware(bool enabled_)
{
	std::lock_guard lock(mutex);

	if (enabled_ == radio_aware)
		return;

	radio_aware = enabled_;

	U_LOG_I("Radio-aware bitrate %s on the headset", radio_aware ? "enabled" : "disabled");

	// The trend is only as good as the reports, which stop when the switch goes off. Never
	// touches the bitrate: the AIMD probes back up on its own if the step was unnecessary.
	flush_radio();
}

uint32_t bitrate_controller::current() const
{
	std::lock_guard lock(mutex);
	return bitrate;
}

uint32_t bitrate_controller::clamp(uint64_t value) const
{
	return uint32_t(std::clamp<uint64_t>(value, min_bitrate, effective_ceiling()));
}

void bitrate_controller::flush()
{
	window.clear();
	healthy_since.reset();
	last_evaluation = {};

	// Every caller of this is a bitrate that just changed. Emptying the window is not enough
	// on its own: the frames already in flight were sent at the old bitrate and would refill
	// it with exactly the samples that were just thrown away, which is how a drain after a
	// probe reads the probe's own frames as congestion. Their delivery rates are still honest
	// measurements of the link and are kept, only their utilisation is misattributed.
	if (has_frames)
		stale_before = newest_frame + 1;
}

void bitrate_controller::flush_radio()
{
	radio_window.clear();
	radio_ema.reset();
	last_radio_sample = {};
	last_radio_step = {};
	radio_hold = false;
	radio_stable_since.reset();
}

void bitrate_controller::flush_estimator()
{
	bandwidth.reset();
	bandwidth_samples = 0;
	last_bandwidth_sample = {};
	bandwidth_peak_since = {};
	bbr_st = bbr_state::startup;
	startup_mark = 0;
	startup_stalled = 0;
	round_started = {};
	direct_probe_gain = gain_probe;
	last_probe = {};
	probe_until = {};
	last_bbr_change = {};
}

int64_t bitrate_controller::min_loaded_wire_ns() const
{
	if (frame_period <= 0)
		return 0;

	// Relative to the paced window when the shards are paced: that window is the shortest
	// time a frame of any size can take, so a fraction of it is the right "did this frame
	// keep the link busy at all" line. With pacing off there is no such floor and the line
	// is a fraction of the frame period instead.
	const double fraction = pacing_window > 0
	                                ? app_limited_wire_fraction * double(pacing_window)
	                                : unpaced_wire_fraction;

	return int64_t(fraction * double(frame_period));
}

std::optional<uint32_t> bitrate_controller::set_ceiling(uint32_t ceiling_bps)
{
	std::lock_guard lock(mutex);

	if (not ceiling_bps)
		return {};

	ceiling = ceiling_bps;
	min_bitrate = std::min(conf.min_bitrate_bps, effective_ceiling());
	return reset_locked();
}

std::optional<uint32_t> bitrate_controller::set_path_ceiling(std::optional<uint32_t> ceiling_bps)
{
	std::lock_guard lock(mutex);

	if (ceiling_bps == path_ceiling)
		return {};

	path_ceiling = ceiling_bps;

	if (not ceiling)
		return {};

	min_bitrate = std::min(conf.min_bitrate_bps, effective_ceiling());

	if (path_ceiling)
		U_LOG_I("Bitrate ceiling for the active path: %.1f Mbit/s (client asked for %.1f Mbit/s)",
		        effective_ceiling() * to_mbits,
		        ceiling * to_mbits);
	else
		U_LOG_I("Bitrate ceiling back to the client's %.1f Mbit/s", ceiling * to_mbits);

	return reset_locked();
}

std::optional<uint32_t> bitrate_controller::set_combined(bool on, uint32_t usb_bps)
{
	std::lock_guard lock(mutex);

	if (combined == on and combined_usb_bps == usb_bps)
		return {};

	combined = on;
	combined_usb_bps = usb_bps;

	if (not ceiling)
		return {};

	min_bitrate = std::min(conf.min_bitrate_bps, effective_ceiling());

	if (combined)
		U_LOG_I("Bitrate control now measuring both paths as one link, ceiling %.1f Mbit/s (USB path budget %.1f Mbit/s, not added to it)",
		        effective_ceiling() * to_mbits,
		        usb_bps * to_mbits);
	else
		U_LOG_I("Bitrate control back to a single path, ceiling %.1f Mbit/s", effective_ceiling() * to_mbits);

	// Everything on record describes the other link, whichever direction this went in
	return reset_locked();
}

uint32_t bitrate_controller::bandwidth_estimate() const
{
	std::lock_guard lock(mutex);

	if (mode_locked() != mode::bbr or not bandwidth.valid() or bandwidth_samples < estimator_min_samples)
		return 0;

	return uint32_t(std::min<double>(bandwidth.get(), std::numeric_limits<uint32_t>::max()));
}

std::optional<uint32_t> bitrate_controller::reset()
{
	std::lock_guard lock(mutex);
	return reset_locked();
}

std::optional<uint32_t> bitrate_controller::reset_locked()
{
	if (not ceiling)
		return {};

	bitrate = effective_ceiling();
	recovery_target = bitrate;
	st = state::steady;
	first_recovery_step = true;
	aimd_probe_ratio = 0.35;
	quality_stream_mask = measured_stream_mask = 0;
	frames = {};
	has_frames = false;
	stale_before = 0;
	flush();

	// A bandwidth estimate is a property of the link, but every caller of this is a change
	// that invalidates it as well: a new ceiling, a new path, a resumed session, a different
	// control law. Starting the ramp over costs a second and cannot be wrong.
	flush_estimator();

	// The radio samples are a property of the radio, not of the bitrate, and survive a reset;
	// the hold does not, the bitrate is back at the ceiling and probing is allowed again.
	radio_hold = false;
	radio_stable_since.reset();
	last_radio_step = {};

	// Forget the emergency half-rate mode too: the measurements that engaged it say nothing
	// about the link the caller is re-seeding for. The session mirrors emergency_framerate_active()
	// onto the compositor, so this clear is what restores the full framerate through that path.
	emergency_active = false;
	emergency_severe_since = {};
	emergency_clean_since = {};

	return bitrate;
}

void bitrate_controller::close_frame(frame_state & frame, clock::time_point now)
{
	if (frame.index == uint64_t(-1))
		return;

	const bool fresh = frame.index >= stale_before;
	const double quality_scale = frame.quality_budget_bps && frame.bytes && frame_period > 0
	                                     ? double(frame.quality_budget_bps) * double(frame.quality_period_ns > 0 ? frame.quality_period_ns : frame_period) / (8e9 * double(frame.bytes))
	                                     : 0;

	if (frame.lost)
	{
		if (fresh)
			window.push_back({.when = now, .lost = true, .late = frame.late, .quality_scale = quality_scale});
	}
	else if (frame.valid and frame.first and frame.last >= frame.first and frame_period > 0)
	{
		// The widest span any ONE stream took, not the span across streams: see
		// frame_state::stream_first for why the difference is the whole bug.
		int64_t wire_ns = 0;
		for (size_t i = 0; i < frame_state::max_streams; ++i)
		{
			const XrTime f0 = frame.stream_first[i], f1 = frame.stream_last[i];
			if (f0 and f1 >= f0)
				wire_ns = std::max(wire_ns, int64_t(f1 - f0));
		}
		// Nothing per-stream (an old feedback with no stream detail): fall back to
		// the merged pair, which is what this always used.
		if (wire_ns == 0)
			wire_ns = int64_t(frame.last - frame.first);

		// Rate bytes and their receive intervals must describe the same streams. The
		// intervals' union excludes gaps between serially delivered eyes without changing
		// the widest-single-stream span used for utilisation above.
		std::array<std::pair<XrTime, XrTime>, frame_state::max_streams> intervals{};
		size_t interval_count = 0;
		uint64_t rate_bytes = 0;
		int64_t rate_loaded_ns = 0;
		for (size_t i = 0; i < frame_state::max_streams; ++i)
		{
			const XrTime first = frame.stream_first[i], last = frame.stream_last[i];
			if (frame.stream_bytes[i] and first > 0 and last > first)
			{
				intervals[interval_count++] = {first, last};
				rate_bytes += frame.stream_bytes[i];
				rate_loaded_ns = std::max(rate_loaded_ns, int64_t(last - first));
			}
		}
		std::sort(intervals.begin(), intervals.begin() + interval_count,
		          [](const auto & a, const auto & b) { return a.first < b.first; });
		uint64_t rate_wire_ns = 0;
		if (interval_count)
		{
			XrTime first = intervals[0].first;
			XrTime last = intervals[0].second;
			for (size_t i = 1; i < interval_count; ++i)
			{
				if (intervals[i].first > last)
				{
					rate_wire_ns += uint64_t(last - first);
					first = intervals[i].first;
					last = intervals[i].second;
				}
				else
					last = std::max(last, intervals[i].second);
			}
			rate_wire_ns += uint64_t(last - first);
		}

		double rate = 0;
		// Only a frame that actually loaded the link says anything about how much the
		// link can carry. Apply the existing threshold to streams that contribute bytes;
		// unmatched long intervals cannot admit a short, app-limited byte sample.
		if (mode_locked() == mode::bbr and rate_loaded_ns >= min_loaded_wire_ns() and rate_bytes and rate_wire_ns > 0)
		{
			rate = 8e9 * double(rate_bytes) / double(rate_wire_ns);
			const bool had_peak = bandwidth.valid();
			const double old_peak = had_peak ? bandwidth.get() : 0;
			bandwidth.update(rate, now, estimator_window);
			if (not had_peak or rate > old_peak)
				bandwidth_peak_since = now;
			++bandwidth_samples;
			last_bandwidth_sample = now;
		}

		if (fresh)
			window.push_back({
			        .when = now,
			        .utilisation = float(double(wire_ns) / double(frame_period)),
			        .late = frame.late,
			        .rate = rate,
			        .quality_scale = quality_scale,
			});
	}

	frame = {};
}

void bitrate_controller::on_frame_bytes(uint64_t frame_index, uint8_t stream_index, uint32_t bytes, clock::time_point now, uint32_t quality_budget_bps, int64_t quality_period_ns)
{
	std::lock_guard lock(mutex);

	// v1 never looks at the byte counts, and letting them touch the frame ring would move
	// the instant a frame becomes a sample for no benefit at all.
	if (mode_locked() != mode::bbr or not conf.enabled or not client_enabled or not ceiling)
		return;

	// Same streams the feedback is taken from: the promoted quad layer reports no timings
	// here, so counting its bytes against a span measured over the eye streams only would
	// read as bandwidth that was never measured. Leaving them out under-counts instead,
	// which can only make the estimate conservative.
	if (stream_index >= video_stream_count or not bytes)
		return;

	if (not has_frames or frame_index > newest_frame)
	{
		newest_frame = frame_index;
		has_frames = true;
	}
	// The feedback for this frame has already been turned into a sample. Can happen after a
	// stall: the send path is normally a frame or two *ahead* of the feedback.
	else if (frame_index + frame_ring_size <= newest_frame)
		return;

	auto & frame = frames[frame_index % frames.size()];
	if (frame.index != frame_index)
	{
		close_frame(frame, now);
		frame.index = frame_index;
	}

	frame.bytes += bytes;
	frame.stream_bytes[stream_index] += bytes;
	frame.quality_budget_bps = quality_budget_bps;
	frame.quality_period_ns = quality_period_ns;
	measured_stream_mask |= uint8_t(1u << stream_index);
	if (quality_budget_bps)
		quality_stream_mask |= uint8_t(1u << stream_index);
	else
		quality_stream_mask &= uint8_t(~(1u << stream_index));
}

bitrate_controller::stats bitrate_controller::analyse(clock::time_point now)
{
	while (not window.empty() and now - window.front().when > window_duration)
		window.pop_front();

	stats res{.count = window.size()};

	std::vector<float> utilisations;
	std::vector<double> rates;
	std::vector<double> peak_epoch_rates;
	utilisations.reserve(window.size());
	for (const auto & s: window)
	{
		if (s.lost)
			++res.lost;
		else
			utilisations.push_back(s.utilisation);
		if (s.late)
			++res.late;
		if (s.rate > 0)
		{
			rates.push_back(s.rate);
			if (bandwidth_peak_since and s.when >= *bandwidth_peak_since)
			{
				peak_epoch_rates.push_back(s.rate);
				++res.peak_epoch_rate_count;
			}
		}
		// Use the most expensive compression ratio in this fresh feedback window.
		if (s.quality_scale > 0)
			res.quality_scale = res.quality_scale > 0 ? std::min(res.quality_scale, s.quality_scale) : s.quality_scale;
	}

	if (not utilisations.empty())
	{
		size_t n = std::min(utilisations.size() - 1,
		                    size_t(utilisation_percentile * utilisations.size()));
		std::nth_element(utilisations.begin(), utilisations.begin() + n, utilisations.end());
		res.utilisation = utilisations[n];
	}

	// Same high percentile, for the same reason: one slow frame is not the link slowing
	// down, and it is the *best* the link has recently managed that a ten second maximum
	// can honestly be compared against.
	res.rate_count = rates.size();
	if (not rates.empty())
	{
		size_t n = std::min(rates.size() - 1, size_t(utilisation_percentile * rates.size()));
		std::nth_element(rates.begin(), rates.begin() + n, rates.end());
		res.rate = rates[n];
	}
	if (not peak_epoch_rates.empty())
	{
		size_t n = std::min(peak_epoch_rates.size() - 1,
		                    size_t(utilisation_percentile * peak_epoch_rates.size()));
		std::nth_element(peak_epoch_rates.begin(), peak_epoch_rates.begin() + n, peak_epoch_rates.end());
		res.peak_epoch_rate = peak_epoch_rates[n];
	}

	// A frame that never arrived is at least as bad as a fully saturated one.
	if (res.lost)
		res.utilisation = std::max(res.utilisation, double(utilisation_severe));

	return res;
}

bitrate_controller::radio_trend bitrate_controller::analyse_radio(clock::time_point now)
{
	while (not radio_window.empty() and now - radio_window.front().when > radio_trend_window)
		radio_window.pop_front();

	radio_trend res;
	if (radio_window.size() < radio_min_samples)
		return res;

	const auto & oldest = radio_window.front();
	const auto & newest = radio_window.back();

	res.span_s = std::chrono::duration<double>(newest.when - oldest.when).count();
	if (res.span_s <= 0)
		return res;

	// Least squares slope of the smoothed level against time, in dB/s. Ordinary means and
	// covariance, the window holds a handful of samples.
	double mean_t = 0;
	double mean_y = 0;
	for (const auto & s: radio_window)
	{
		mean_t += std::chrono::duration<double>(s.when - oldest.when).count();
		mean_y += s.rssi_dbm;
	}
	mean_t /= double(radio_window.size());
	mean_y /= double(radio_window.size());

	double cov = 0;
	double var = 0;
	for (const auto & s: radio_window)
	{
		double dt = std::chrono::duration<double>(s.when - oldest.when).count() - mean_t;
		cov += dt * (s.rssi_dbm - mean_y);
		var += dt * dt;
	}
	if (var <= 0)
		return res;

	res.usable = true;
	res.rssi_dbm = newest.rssi_dbm;
	res.slope_db_per_s = cov / var;
	res.delta_db = res.slope_db_per_s * res.span_s;
	res.link_speed_mbps = newest.link_speed_mbps;
	for (const auto & s: radio_window)
		res.link_speed_peak_mbps = std::max(res.link_speed_peak_mbps, s.link_speed_mbps);

	return res;
}

std::optional<uint32_t> bitrate_controller::on_wifi_state(int rssi_dbm, int link_speed_mbps, clock::time_point when)
{
	std::lock_guard lock(mutex);

	// Both switches, plus the radio one, plus a stream to control at all.
	if (not conf.enabled or not client_enabled or not radio_aware or not ceiling)
		return {};

	// Sentinels and nonsense. Android answers -127 when it will not say, and no real Wi-Fi
	// link sits at 0 dBm or below -110 dBm.
	if (rssi_dbm >= 0 or rssi_dbm <= -110)
		return {};

	// A gap in the reports means the trend across it is meaningless: start a new one.
	if (not radio_window.empty() and when - last_radio_sample > radio_max_age)
	{
		radio_window.clear();
		radio_ema.reset();
		radio_hold = false;
		radio_stable_since.reset();
	}
	last_radio_sample = when;

	radio_ema = radio_ema
	                    ? radio_ema_alpha * rssi_dbm + (1 - radio_ema_alpha) * *radio_ema
	                    : double(rssi_dbm);
	radio_window.push_back({
	        .when = when,
	        .rssi_dbm = *radio_ema,
	        .link_speed_mbps = std::max(0, link_speed_mbps),
	});

	auto trend = analyse_radio(when);
	if (not trend.usable)
		return {};

	// The deep drop and its rebound are a closed loop of their own; a guess from the radio
	// on top of it would only make the two fight. v2 has no such regime: its estimate is
	// always in charge and the radio only changes the gain applied to it.
	if (mode_locked() == mode::aimd and st == state::recovering)
		return {};

	// The signal is coming back: let the normal probing upwards resume at once.
	if (radio_hold and trend.delta_db > radio_rise_db and trend.rssi_dbm > radio_low_rssi_dbm)
	{
		radio_hold = false;
		U_LOG_I("Radio-aware bitrate: signal recovering (%+.1f dB over %.1f s, now %.0f dBm), probing allowed again",
		        trend.delta_db,
		        trend.span_s,
		        trend.rssi_dbm);
	}

	// Trigger 1: a real fall, and low enough that the fall has nothing left to eat into.
	const bool falling = trend.delta_db < -radio_fall_db and trend.rssi_dbm < radio_low_rssi_dbm;
	// Trigger 2: the radio's own rate adaptation already gave up half of the PHY rate, and
	// what is left no longer has room for what is being sent.
	const bool starved = trend.link_speed_mbps > 0 and
	                     trend.link_speed_peak_mbps > 0 and
	                     double(trend.link_speed_mbps) <= radio_link_speed_collapse * double(trend.link_speed_peak_mbps) and
	                     double(trend.link_speed_mbps) * 1e6 < radio_link_speed_headroom * double(bitrate);

	// A hold taken on a fall must also let go when the fall simply *stops*: the user walked
	// to a new spot and settled there, the signal is lower but steady, reports keep coming
	// and nothing is congested. The rise-based release above never fires for that (there is
	// no rise back up), so without this the hold would latch forever and block every probe.
	// Release once the slope has stayed flat — neither trigger active, |slope| below
	// radio_stable_slope — for a sustained radio_stable_hold. While the signal is genuinely
	// still falling the flat test is false and the timer never starts, so the hold stays.
	if (radio_hold and not falling and not starved and
	    std::abs(trend.slope_db_per_s) < radio_stable_slope)
	{
		if (not radio_stable_since)
			radio_stable_since = when;
		else if (when - *radio_stable_since >= radio_stable_hold)
		{
			radio_hold = false;
			radio_stable_since.reset();
			U_LOG_I("Radio-aware bitrate: signal stable at %.0f dBm (%.2f dB/s over %.1f s), probing allowed again",
			        trend.rssi_dbm,
			        trend.slope_db_per_s,
			        trend.span_s);
		}
	}
	else
		radio_stable_since.reset();

	if (not falling and not starved)
		return {};

	// Same cooldowns as any other decrease, plus one of its own: a preemptive step is a
	// guess and the frame timings must be given time to confirm or deny it.
	if (when - last_decrease < decrease_cooldown or when - last_radio_step < radio_step_interval)
		return {};

	const uint32_t previous = bitrate;

	// v2 expresses the same preemptive step as its own gain: it is the estimate that is
	// about to be wrong, and the radio-degrading gain is what the next evaluation would
	// apply anyway, so taking it here just brings it forward. The estimate itself is left
	// alone — the radio is a guess about the future, not a measurement of the link.
	if (mode_locked() == mode::bbr and bandwidth.valid())
	{
		bbr_st = bbr_state::steady;
		// Never upwards, whatever the estimate says: same rule as v1.
		const uint32_t target = clamp(uint64_t(gain_radio * bandwidth.get()));
		if (target < bitrate)
		{
			bitrate = target;
			last_bbr_change = when;
		}
	}
	else
		bitrate = clamp(uint64_t(bitrate * decrease_factor));

	// Hold the probing back up until the radio says the degradation is over, whether or not
	// the bitrate could actually move (it may already be on the floor).
	radio_hold = true;
	last_decrease = when;
	last_radio_step = when;

	if (bitrate == previous)
		return {};

	// The utilisation samples were taken at the old bitrate and say nothing about the new one.
	flush();

	U_LOG_I("Automatic bitrate: radio degrading, %.1f -> %.1f Mbit/s (%s: RSSI %.0f dBm, %+.1f dB over %.1f s, %.1f dB/s, link %d Mbit/s, peak %d Mbit/s)",
	        previous * to_mbits,
	        bitrate * to_mbits,
	        falling ? "falling signal" : "PHY rate collapse",
	        trend.rssi_dbm,
	        trend.delta_db,
	        trend.span_s,
	        trend.slope_db_per_s,
	        trend.link_speed_mbps,
	        trend.link_speed_peak_mbps);

	return bitrate;
}

std::optional<uint32_t> bitrate_controller::on_feedback(const from_headset::feedback & feedback, int64_t period_ns, bool streaming, clock::time_point now)
{
	std::lock_guard lock(mutex);

	if (not conf.enabled or not client_enabled or not streaming or not ceiling or period_ns <= 0)
		return {};

	// Only the video streams (one per encoder) report frame delivery timings.
	if (feedback.stream_index >= video_stream_count)
		return {};

	frame_period = period_ns;

	if (not has_frames or feedback.frame_index > newest_frame)
	{
		newest_frame = feedback.frame_index;
		has_frames = true;
	}
	// Feedback for a frame that has already been turned into a sample (the client sends several
	// packets per frame, the last one only after it was displayed).
	else if (feedback.frame_index + frame_ring_size <= newest_frame)
		return {};

	auto & frame = frames[feedback.frame_index % frames.size()];
	if (frame.index != feedback.frame_index)
	{
		close_frame(frame, now);
		frame.index = feedback.frame_index;
	}

	if (not feedback.sent_to_decoder)
	{
		// The frame was given up on before it was complete: packets were lost or arrived far
		// too late. Its received_first_packet is the time it was abandoned, not a wire time.
		frame.lost = true;
	}
	else
	{
		if (feedback.received_first_packet)
			frame.first = frame.first ? std::min(frame.first, feedback.received_first_packet) : feedback.received_first_packet;
		if (feedback.received_last_packet)
			frame.last = std::max(frame.last, feedback.received_last_packet);
		// And the same pair for THIS stream alone, which is what the wire span is
		// measured from. See frame_state::stream_first.
		if (feedback.stream_index < frame_state::max_streams)
		{
			auto & sf = frame.stream_first[feedback.stream_index];
			auto & sl = frame.stream_last[feedback.stream_index];
			if (feedback.received_first_packet)
				sf = sf ? std::min(sf, feedback.received_first_packet) : feedback.received_first_packet;
			if (feedback.received_last_packet)
				sl = std::max(sl, feedback.received_last_packet);
		}
		frame.valid = true;
	}

	// Decoded, then evicted by a newer frame without ever being shown. Note that
	// times_displayed == 0 on its own means nothing: it is also the state of the feedback sent
	// as soon as a frame is handed to the decoder.
	if (feedback.received_from_decoder and not feedback.blitted and feedback.times_displayed == 0)
		frame.late = true;

	return evaluate(now);
}

std::optional<uint32_t> bitrate_controller::evaluate(clock::time_point now)
{
	if (now - last_evaluation < evaluation_interval)
		return {};
	last_evaluation = now;

	// The headset stopped reporting its radio: a hold taken on data this old would last
	// forever. Stale radio data does nothing at all, including nothing to the probing.
	if (radio_hold and now - last_radio_sample > radio_max_age)
	{
		radio_hold = false;
		U_LOG_I("Radio-aware bitrate: no Wi-Fi report for %d ms, releasing the hold",
		        int(radio_max_age.count()));
	}

	auto s = analyse(now);
	if (s.count < min_samples)
		return {};

	// The last-resort rung, evaluated from the same window: it reads the bitrate reached by
	// the previous evaluations, so it sees "pinned at the floor" only once the ordinary
	// control has actually taken the bitrate all the way down and it is still not enough.
	update_emergency(now, s);

	return mode_locked() == mode::bbr ? evaluate_bbr(now, s) : evaluate_aimd(now, s);
}

void bitrate_controller::update_emergency(clock::time_point now, const stats & s)
{
	if (not emergency_enabled)
	{
		// Feature off (either switch): make sure the mode is not latched on. The session
		// mirrors emergency_framerate_active() onto the compositor, so clearing it here is
		// enough to restore the full framerate.
		if (emergency_active)
		{
			emergency_active = false;
			emergency_last_change = now;
			U_LOG_I("Emergency half-rate: disabled mid-session, restoring full framerate");
		}
		emergency_severe_since = {};
		emergency_clean_since = {};
		return;
	}

	// Same verdicts the AIMD law uses, kept mode independent here so the last resort behaves
	// the same whichever control law is in force -- including that a frame decoded and
	// then replaced before it was shown only counts as congestion when the link also
	// dropped something. The emergency mode halves the frame rate, and doing that
	// because a saturated client GPU could not show every frame makes it worse.
	const bool severe = s.utilisation > utilisation_severe or
	                    s.lost >= lost_frames_severe or
	                    (s.lost > 0 and s.late >= late_frames_severe);
	const bool healthy = s.utilisation < utilisation_increase and s.lost == 0 and s.late == 0;
	const bool at_floor = bitrate <= min_bitrate;

	if (not emergency_active)
	{
		// Engage only while pinned at the floor and still severely congested: by this point
		// the error correction, the retransmissions, the intra refresh and every bitrate drop
		// have already been tried and the link is still not keeping up.
		if (at_floor and severe)
		{
			if (emergency_severe_since == clock::time_point{})
				emergency_severe_since = now;
			else if (now - emergency_severe_since >= emergency_trigger_duration and
			         now - emergency_last_change >= emergency_min_dwell)
			{
				emergency_active = true;
				emergency_last_change = now;
				emergency_clean_since = {};
				U_LOG_I("Emergency half-rate: engaging, halving the stream framerate (pinned at %u bps, sustained loss)", bitrate);
			}
		}
		else
			emergency_severe_since = {};
	}
	else
	{
		// Restore once the link has been clean for the whole hysteresis window.
		if (healthy)
		{
			if (emergency_clean_since == clock::time_point{})
				emergency_clean_since = now;
			else if (now - emergency_clean_since >= emergency_clear_duration and
			         now - emergency_last_change >= emergency_min_dwell)
			{
				emergency_active = false;
				emergency_last_change = now;
				emergency_severe_since = {};
				U_LOG_I("Emergency half-rate: link clean for %d ms, restoring full framerate", int(emergency_clear_duration.count()));
			}
		}
		else
			emergency_clean_since = {};
	}
}

void bitrate_controller::set_emergency_enabled(bool enabled)
{
	std::lock_guard lock(mutex);
	if (emergency_enabled == enabled)
		return;
	emergency_enabled = enabled;
	emergency_severe_since = {};
	emergency_clean_since = {};
	if (not enabled and emergency_active)
	{
		emergency_active = false;
		emergency_last_change = clock::now();
		U_LOG_I("Emergency half-rate: disabled, restoring full framerate");
	}
}

bool bitrate_controller::emergency_framerate_active() const
{
	std::lock_guard lock(mutex);
	return emergency_active;
}

std::optional<uint32_t> bitrate_controller::evaluate_aimd(clock::time_point now, const stats & s)
{
	// A frame the client decoded and then replaced before showing it is a statement
	// about the CLIENT keeping up, not about the link. On a codec whose headset drops
	// frames at a bounded decode queue by design, it is the normal state of a healthy
	// link -- and lowering the bitrate cannot fix a GPU that is saturated. Live, this
	// alone would have called congestion on a window with zero lost datagrams.
	//
	// So `late` only counts toward a DECREASE when the same window also shows the link
	// dropping something. It still counts against `healthy` below, so a client that is
	// not keeping up stops the controller climbing; it just may not push it down.
	const bool link_lost_something = s.lost > 0;
	const size_t late_for_decrease = link_lost_something ? s.late : 0;
	const bool utilisation_congestion = not aimd_loss_only and s.utilisation > utilisation_decrease;
	const bool span_severe = not aimd_loss_only and s.utilisation > bitrate_controller::utilisation_severe;
	const bool severe = span_severe or
	                    s.lost >= lost_frames_severe or
	                    late_for_decrease >= late_frames_severe;
	const bool degraded = severe or
	                      utilisation_congestion or
	                      s.lost >= lost_frames_decrease or
	                      late_for_decrease >= late_frames_decrease;
	// This mode tolerates receive scheduling that spans almost one refresh.
	// Requiring the ordinary 0.60 threshold here prevents recovery indefinitely.
	// Keep a bounded span and zero lost/late frames before probing upward.
	const double recovery_span = aimd_loss_only ? 1.10 : utilisation_increase;
	const bool healthy = s.utilisation < recovery_span and s.lost == 0 and s.late == 0;

	const uint32_t previous = bitrate;
	const char * reason = nullptr;

	if (degraded)
	{
		healthy_since.reset();
		// A failed probe earns a small retry; clean probes grow back to full speed.
		if (aimd_loss_only)
			aimd_probe_ratio = 0.10;

		if (now - last_decrease < aimd_decrease_cooldown)
			return {};
		last_decrease = now;

		if (st == state::recovering)
		{
			// Congestion returned while rebounding: the pre-drop bitrate was too
			// optimistic, aim lower next time.
			recovery_target = clamp(uint64_t(recovery_target * recovery_target_backoff));
		}

		if (severe)
		{
			if (st != state::recovering)
				recovery_target = bitrate;
			bitrate = clamp(uint64_t(bitrate * deep_decrease_factor));
			st = state::recovering;
			first_recovery_step = true;
			reason = "acute congestion";
			// The deep drop and its rebound are in charge from here; the radio must
			// neither hold the rebound back nor step on top of it.
			radio_hold = false;
		}
		else
		{
			bitrate = clamp(uint64_t(bitrate * decrease_factor));
			reason = "congestion";
		}

		if (st == state::recovering and recovery_target <= bitrate)
			st = state::steady;

		flush();
	}
	else if (healthy)
	{
		if (not healthy_since)
			healthy_since = now;

		auto held = now - *healthy_since;

		if (st == state::recovering)
		{
			const auto hold = aimd_loss_only ? std::chrono::milliseconds{250} :
			                  (first_recovery_step ? recovery_confirm : recovery_step_interval);
			if (held < hold)
				return {};

			const double factor = aimd_loss_only ? 1.0 + aimd_probe_ratio : recovery_factor;
			bitrate = std::min(recovery_target, clamp(uint64_t(bitrate * factor)));
			first_recovery_step = false;
			reason = "link healthy again, rebounding";

			if (bitrate >= recovery_target)
				st = state::steady;
		}
		else
		{
			// Frame timings look fine, but the radio says the signal is still on the
			// way down: probing back up now only walks into the degradation again.
			if (radio_hold)
				return {};

			const auto hold = aimd_loss_only ? std::chrono::milliseconds{250} : increase_hold;
			if (held < hold or bitrate >= effective_ceiling())
				return {};

			uint32_t step = std::max<uint32_t>(increase_step_min, uint32_t(effective_ceiling() * increase_step_ratio));
			// Clean loss-only feedback permits a proportional probe after a deep cut.
			// Keep ordinary AIMD additive and retain the ceiling and radio hold.
			if (aimd_loss_only)
				step = std::max(step, uint32_t(bitrate * aimd_probe_ratio));
			bitrate = clamp(uint64_t(bitrate) + step);
			recovery_target = bitrate;
			reason = "spare capacity";
		}

		if (aimd_loss_only and bitrate > previous)
			aimd_probe_ratio = std::min(0.35, aimd_probe_ratio * 2);
		flush();
	}
	else
	{
		// Between the two thresholds: hysteresis band, hold the current bitrate.
		healthy_since.reset();
		return {};
	}

	if (bitrate == previous)
		return {};

	U_LOG_I("Automatic bitrate: %s, %.1f -> %.1f Mbit/s (p%d utilisation %.2f, %zu lost, %zu late over %zu frames)",
	        reason,
	        previous * to_mbits,
	        bitrate * to_mbits,
	        int(utilisation_percentile * 100),
	        s.utilisation,
	        s.lost,
	        s.late,
	        s.count);

	return bitrate;
}

std::optional<uint32_t> bitrate_controller::evaluate_bbr(clock::time_point now, const stats & s)
{
	// Keep the estimator in measured network units. Only the encoder target is
	// converted for the direct stereo stream whose budget describes image detail.
	const bool quality_mode = quality_stream_mask == 1 && measured_stream_mask == 1;
	const bool missing_scale = quality_mode && s.quality_scale <= 0;
	const double quality_scale = quality_mode ? s.quality_scale : 1.0;

	// The measurements have not caught up with the last change yet, see change_settle.
	if (last_bbr_change != clock::time_point{} and now - last_bbr_change < change_settle)
		return {};

	// Congestion signal: the link handing the same bytes over more slowly than the best it
	// has managed in the last ten seconds. Scale-free, offset-free, see the discussion in
	// the header. Not applied during the startup ramp, where the bitrate is deliberately
	// climbing faster than a two second window can follow.
	const bool peak_confirmed = s.peak_epoch_rate_count >= estimator_min_samples;
	const double slowdown_rate = quality_mode ? s.rate : (peak_confirmed ? s.peak_epoch_rate : 0);
	const double slowdown = (bandwidth.valid() and slowdown_rate > 0) ? bandwidth.get() / slowdown_rate : 1;

	// A probe is a deliberate overshoot: on a link that is already the bottleneck it stretches
	// the frames past a frame period on purpose, and reading that back as congestion would
	// turn every probe into a backoff. Frames actually lost or dropped still count.
	const bool overshooting = bbr_st == bbr_state::probe;
	// Same rule as the AIMD law: a frame decoded and dropped before it was shown is
	// the client not keeping up, and only counts as congestion alongside real loss.
	const bool acute = s.lost >= lost_frames_decrease or
	                   (s.lost > 0 and s.late >= late_frames_decrease) or
	                   (not overshooting and
	                    (s.utilisation > (quality_mode ? 1.10 : utilisation_severe) or
	                     (bbr_st != bbr_state::startup and slowdown > slowdown_backoff and
	                      (quality_mode or peak_confirmed) and
	                      (not quality_mode or s.utilisation > 1.10))));

	// An estimate no loaded frame has refreshed for a whole window is not a bottleneck any
	// more: the link has been carrying everything asked of it without ever filling up. Forget
	// it rather than hold a bitrate down against a limit that stopped existing.
	if (bandwidth_samples and now - last_bandwidth_sample > estimator_window)
	{
		bandwidth.reset();
		bandwidth_samples = 0;
		bandwidth_peak_since = {};
		U_LOG_I("Automatic bitrate v2: no loaded frame for %d ms, dropping the bandwidth estimate",
		        int(estimator_window.count()));
	}

	// Lost frames can have no byte callback at all. Missing conversion metadata
	// must never hide that loss, but cannot justify a clean target or a fake rate.
	if (missing_scale and not acute)
		return {};

	if (not missing_scale and bandwidth_samples < estimator_min_samples)
	{
		// Nothing measured. On a link with capacity to spare every frame is over before
		// it loaded anything (see the app-limited rule) and there is simply nothing to
		// estimate. There is still one thing worth doing: if the link measures healthy
		// and the bitrate is below the ceiling, walk it back up. No bottleneck was ever
		// found, so the only defensible target is the bitrate that was asked for — and
		// with no estimate to size a step from, v1's blind additive probe is exactly the
		// right fallback, hold and all.
		if (not acute)
		{
			const bool healthy = s.utilisation < (quality_mode ? 1.10 : utilisation_increase) and s.lost == 0 and s.late == 0;
			if (not healthy or radio_hold)
			{
				healthy_since.reset();
				return {};
			}

			if (not healthy_since)
				healthy_since = now;

			if (now - *healthy_since < increase_hold or bitrate >= effective_ceiling())
				return {};

			const uint32_t before = bitrate;
			const uint32_t step = std::max<uint32_t>(increase_step_min, uint32_t(effective_ceiling() * increase_step_ratio));
			bitrate = clamp(uint64_t(bitrate) + step);
			flush();

			if (bitrate == before)
				return {};

			U_LOG_I("Automatic bitrate v2: no bottleneck measured, %.1f -> %.1f Mbit/s (p%d utilisation %.2f over %zu frames)",
			        before * to_mbits,
			        bitrate * to_mbits,
			        int(utilisation_percentile * 100),
			        s.utilisation,
			        s.count);
			return bitrate;
		}

		// ... unless the link just failed, which is a measurement of its own: whatever is
		// being sent is more than it can carry. Seed the filter with it so that the
		// backoff below has something to work from, and so that there is an estimate from
		// here on.
		bandwidth.reset();
		bandwidth.update(double(bitrate) / quality_scale, now, estimator_window);
		bandwidth_peak_since = now;
		bandwidth_samples = estimator_min_samples;
		last_bandwidth_sample = now;
	}

	const double bw = bandwidth.get();
	const uint32_t previous = bitrate;
	const char * reason = nullptr;
	double gain = gain_steady;
	// A state change the bitrate has to follow at once, whatever the steady rate limiting
	// would otherwise say
	bool forced = false;

	if (acute)
	{
		if (now - last_decrease < (quality_mode ? aimd_decrease_cooldown : decrease_cooldown))
			return {};
		last_decrease = now;
		// A direct-stream probe need not repeat a large overshoot after real congestion.
		if (quality_mode)
			direct_probe_gain = 1.04;

		// A maximum filter remembers for ten seconds, and an acute failure is proof that
		// what it remembers was never really deliverable. Replace it by what the link is
		// measurably doing right now, or, when every recent frame was app-limited and
		// there is no such measurement, simply take a bite out of it.
		bandwidth.cap(s.rate > 0 ? s.rate : backoff_factor * bw);
		bandwidth_peak_since = now;

		bbr_st = bbr_state::steady;
		startup_stalled = startup_stall_rounds;
		last_probe = now;
		last_bbr_change = now;
		probe_until = {};

		bitrate = missing_scale ? clamp(uint64_t(previous * backoff_factor)) :
		                          clamp(uint64_t(std::min(backoff_factor * bandwidth.get() * quality_scale, double(effective_ceiling()))));
		// A burst can inflate the delivery estimate while frames are still lost.
		// Acute backoff must reduce the current rate, never rebound above it.
		bitrate = std::min(bitrate, clamp(uint64_t(previous * backoff_factor)));
		gain = backoff_factor;
		reason = "backing off";

		// Samples taken at the old bitrate say nothing about the new one, same as v1.
		flush();
	}
	else
	{
		// The radio is the only leading indicator there is: it says the estimate is about
		// to be too optimistic. Leave the ramp and the probing alone until it recovers.
		if (radio_hold and bbr_st != bbr_state::steady)
		{
			bbr_st = bbr_state::steady;
			probe_until = {};
			last_probe = now;
		}

		switch (bbr_st)
		{
			case bbr_state::startup: {
				if (round_started == clock::time_point{})
				{
					round_started = now;
					startup_mark = bw;
				}
				else if (now - round_started >= round_duration)
				{
					startup_stalled = bw > startup_growth * startup_mark ? 0 : startup_stalled + 1;
					startup_mark = bw;
					round_started = now;
				}

				if (startup_stalled >= startup_stall_rounds)
				{
					bbr_st = bbr_state::steady;
					last_probe = now;
					U_LOG_I("Automatic bitrate v2: startup done, bandwidth plateaued at %.1f Mbit/s after %zu flat rounds",
					        bw * to_mbits,
					        startup_stalled);
				}
				break;
			}

			case bbr_state::probe:
				if (now >= probe_until)
				{
					// Grow only when the raised stream still fits inside a refresh period.
					if (quality_mode and s.lost == 0 and s.late == 0 and s.utilisation < 1.0)
						direct_probe_gain = std::min(gain_probe, direct_probe_gain + 0.02);
					bbr_st = bbr_state::steady;
					last_probe = now;
					// The samples in the window were taken at the raised
					// gain; the drain back to the steady one must not wait
					// for them to age out, nor for the steady interval.
					flush();
					forced = true;
				}
				break;

			case bbr_state::steady:
				// One probe every probe_interval, to rediscover capacity that came
				// back. Never into a falling radio: that is walking into the wall v1
				// used to walk into with its blind additive increase.
				if (not radio_hold and now - last_probe >= (quality_mode ? std::chrono::milliseconds{500} : probe_interval) and bitrate < effective_ceiling())
				{
					bbr_st = bbr_state::probe;
					probe_until = now + probe_duration;
					U_LOG_I("Automatic bitrate v2: probing at gain %.2f, estimate %.1f Mbit/s",
					        quality_mode ? direct_probe_gain : gain_probe,
					        bw * to_mbits);
				}
				break;
		}

		switch (bbr_st)
		{
			case bbr_state::startup:
				gain = gain_startup;
				reason = "startup";
				break;
			case bbr_state::probe:
				gain = quality_mode ? direct_probe_gain : gain_probe;
				reason = "probing";
				break;
			case bbr_state::steady:
				gain = radio_hold ? gain_radio : gain_steady;
				reason = radio_hold ? "steady, radio degrading" : "steady";
				break;
		}

		uint32_t target = clamp(uint64_t(std::min(gain * bw * quality_scale, double(effective_ceiling()))));
		// Coalesced receive timestamps can make a clean, compressed frame look
		// link-limited. Without congestion or radio evidence, do not spend quality
		// merely to match that estimate. Decoder lateness also blocks upward probes.
		if (quality_mode and not radio_hold)
			target = s.late ? bitrate : std::max(bitrate, target);
		// A fresh native peak may raise bandwidth.max before its post-peak cohort
		// confirms that the rate is repeatable. Hold only steady upward target changes;
		// startup, probe completion, direct quality, and all decreases stay unchanged.
		if (not quality_mode and bbr_st == bbr_state::steady and not forced and not peak_confirmed and target > bitrate)
			target = bitrate;

		// Do not chase the few percent the estimate wobbles by, and do not re-encode at a
		// new bitrate more often than the selected steady interval after startup.
		if (bbr_st != bbr_state::startup and not forced)
		{
			if (now - last_bbr_change < (quality_mode ? std::chrono::milliseconds{500} : steady_interval))
				return {};

			const double delta = std::abs(double(target) - double(bitrate));
			if (bitrate and delta < steady_change_threshold * double(bitrate) and
			    not (quality_mode and target > bitrate and (target == effective_ceiling() or bbr_st == bbr_state::probe)))
				return {};
		}

		bitrate = target;
		last_bbr_change = now;

		// Samples taken at the old bitrate say nothing about the new one, same as v1. The
		// bandwidth filter is deliberately not flushed: it is the one thing that has to
		// survive a change of bitrate.
		if (bitrate != previous)
			flush();
	}

	if (bitrate == previous)
		return {};

	U_LOG_I("Automatic bitrate v2: %s, %.1f -> %.1f Mbit/s (estimate %.1f Mbit/s, gain %.2f, recent %.1f Mbit/s over %zu samples, slowdown x%.2f, p%d utilisation %.2f, %zu lost, %zu late over %zu frames)",
	        reason,
	        previous * to_mbits,
	        bitrate * to_mbits,
	        bandwidth.get() * to_mbits,
	        gain,
	        s.rate * to_mbits,
	        s.rate_count,
	        slowdown,
	        int(utilisation_percentile * 100),
	        s.utilisation,
	        s.lost,
	        s.late,
	        s.count);

	return bitrate;
}

} // namespace wivrn
