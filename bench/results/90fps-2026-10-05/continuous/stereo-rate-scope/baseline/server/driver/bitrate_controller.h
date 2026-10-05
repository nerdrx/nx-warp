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

#pragma once

#include "wivrn_packets.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace wivrn
{

// Automatic bitrate control.
//
// The client acknowledges every video frame with a from_headset::feedback packet that carries,
// among others, the time the first and the last packet of that frame were received (both in the
// client clock, so their difference needs no clock offset). The fraction of a frame period spent
// receiving a frame is a direct measure of how much of the link capacity the stream is using:
//
//     utilisation = (received_last_packet - received_first_packet) / frame_period
//
// A utilisation close to (or above) 1 means the wireless link cannot deliver a frame within the
// time budget of a frame, which is exactly what "the connection lags" feels like. Frames that
// never arrive completely (no sent_to_decoder, same test the IDR handler uses) and frames that are
// decoded but dropped before ever being shown are counted as delivery failures.
//
// Two regimes coexist:
//   * gradual degradation (walking away from the router): gentle multiplicative decrease followed
//     by slow additive probing back up;
//   * acute lag spike: repeated bounded cuts allow the queue to drain, then
//     smaller, frequent recovery steps approach the pre-drop bitrate. If congestion returns during
//     the rebound the rebound target itself is lowered (the classic ssthresh idea) so the two do
//     not oscillate.
//
// All decisions are taken from a sliding window of per-frame samples, using a high percentile so
// that a single unlucky frame does not move the bitrate. The window is flushed after every change
// so that stale samples cannot trigger a second change.
//
// On top of that, the headset reports its Wi-Fi radio state about once a second
// (from_headset::wifi_state). Frame timings are a *lagging* indicator: by the time the utilisation
// rises the packets are already late. The radio is a *leading* one — the signal starts falling a
// second or two before the rate adaptation gives up and the first packet is lost. The controller
// therefore steps down preemptively on a falling signal, while the frame timings still look fine.
// Only the trend is used: absolute dBm says nothing portable, a fall of several dB over a few
// seconds says the user is walking away from the access point. Radio input can only ever *lower*
// the bitrate; it never raises one, and it never runs while the deep-drop recovery is in charge.
//
// ======================================================================================
// Control law v2: delivered-bandwidth estimation (bitrate_mode::bbr)
// ======================================================================================
//
// Everything above is a *congestion* controller: it reacts to the link being full. It never
// learns how big the link is, so after a decrease it has to walk back up blind, one additive
// step after a healthy interval, and after a deep drop it rebounds towards a number it remembered
// rather than one it measured. v2 measures the link instead, the way BBR does, and derives the
// bitrate from that estimate. It is selected per session and runs inside this same object: the
// ceilings, the floor, the two switches, the frame ring that joins the per-stream feedback into
// one frame, the acute lost/late detection and the radio trend are all shared and all still
// apply — only the function that turns a window of samples into a bitrate differs.
//
// --- The delivery rate sample ---------------------------------------------------------
// The headset says, per frame, when the first and the last packet of that frame arrived. The
// server knows how many bytes it put on the wire for that frame (see on_frame_bytes: the
// encoder reports them from its send path, parity shards included). For NX direct this is
// datagram payload accounting, excluding outer WiVRn framing and tail padding. One frame yields
//
//     delivery_rate = 8 * frame_bytes / (received_last - received_first)
//
// NX direct's controller input buys image detail before lossless compression; it is not
// a wire-rate target. The encoder tags each frame with that input and its cadence. Only
// the control output is converted through their ratio to the actual datagram bytes.
// The delivery-rate estimator never receives the nominal quality budget as a byte count.
//
// which is a *lower bound* on the capacity of the bottleneck: those bytes really did get
// through in that time. Both timestamps are in the client clock, so no clock offset is needed.
//
// --- App-limited samples --------------------------------------------------------------
// A lower bound is only useful if the sender was actually trying. BBR calls a sample
// "app-limited" when the application had nothing more to send, and refuses to let such a
// sample lower its estimate. Here the analogous case is a frame that is small compared to what
// the link could have carried in that window: a nearly static scene produces a 5 kB P-frame
// that goes out in one micro-burst and lands in a few hundred microseconds, and 5 kB over
// 300 us reads as 130 Mbit/s of "capacity" that is really one Wi-Fi TXOP measured with a
// stopwatch. Feeding that to a *maximum* filter is worse than useless, so a sample is only
// admitted when the frame occupied the link for a meaningful stretch of the frame period:
// see app_limited_wire_fraction. Frames below it are still used for utilisation, loss and
// lateness — they just say nothing about capacity.
//
// Packet pacing (see shard_pacer) is the other half of the same story, and the reason the
// threshold is expressed relative to the *paced* window rather than to a frame period. Pacing
// deliberately spreads a frame over ~40% of a frame period whatever its size, so with pacing on
// the measured delivery rate saturates at about bitrate / window: the sender, not the link, is
// what limits it. That is a genuine app-limited regime and it is handled the way BBR handles
// its own: the sample is still a valid lower bound, the estimate climbs to it, and the
// controller keeps raising the bitrate until something else stops it — a ceiling, or the link
// actually filling up, at which point the receive span stretches past the paced window and the
// samples become real capacity measurements again. Pacing therefore costs v2 the ability to
// measure headroom it is not using, which is precisely what pacing is for.
//
// --- The queueing signal, and why it is not an RTT --------------------------------------
// BBR's second state variable is min_rtt, and the ratio of the current RTT to it is how it
// notices a queue building before anything is lost. A real RTT is not available here without
// a clock offset: send_begin is in the server clock and received_first_packet in the client's,
// and while the server does run a clock offset estimator, it needs seconds to converge, it is
// reset on every path failover, and its residual error is of the same order as the queueing
// delay that would have to be detected. Depending on it would make the controller useless
// exactly when adaptation matters most — the first seconds of a session, and right after a
// failover. So no absolute delay is used anywhere in v2.
//
// The obvious offset-free substitute is the wire span itself, received_last - received_first,
// compared against its own windowed minimum. That is wrong, and worth spelling out: a wire
// span is a *transmission* time, not a propagation delay. It is proportional to the number of
// bytes in the frame, so it grows whenever the bitrate is raised or the scene gets busy, with
// no queue anywhere — on an unpaced link the span at 50 Mbit/s is simply twice the span at
// 25 Mbit/s. A min filter on it would read every increase as congestion.
//
// Dividing by the bytes removes exactly that dependency, and what is left is the delivery rate
// the estimator already computes. So the queueing signal is
//
//     slowdown = windowed_max_bandwidth / (recent delivery rate)
//
// — the link handing over the same bytes more slowly than the best it has managed in the last
// ten seconds. It is scale-free (a bigger frame takes proportionally longer and the ratio does
// not move), it needs no clock offset, and it is the same quantity in both pacing regimes: with
// pacing on both numerator and denominator sit at bitrate / window until the link becomes the
// bottleneck, at which point only the denominator falls. Both sides are high percentiles, so a
// single slow frame does not trigger it, and it is ignored during the startup ramp, where the
// bitrate is deliberately climbing faster than the measurement can follow.
//
// --- The state machine -------------------------------------------------------------------
// startup: gain 1.25, doubling-ish growth until the bandwidth estimate stops improving for
//          three consecutive rounds — BBR's STARTUP and its bandwidth-plateau exit.
// steady:  gain 0.85. The gap to 1 is the headroom that keeps the bottleneck queue empty; it
//          is also what leaves room for the P-frame/I-frame variance a video encoder produces
//          around its nominal bitrate.
// probe:   once every eight seconds, one round at gain 1.10, so that capacity that came back
//          (the user walked towards the router again) is rediscovered instead of waiting for
//          the ten second maximum filter to age out. Skipped while the radio says the link is
//          degrading: probing into a fall is how v1's blind additive increase used to hurt.
//          A probe is a deliberate overshoot — on a link that is already the bottleneck it
//          will by construction stretch the frames past a frame period — so while one is
//          running its own overshoot is not read as congestion. Only frames actually lost or
//          dropped are, those are never worth a probe. Returning to steady right afterwards
//          is the drain, and it happens without waiting out the steady interval.
// backoff: on the same acute signals v1 treats as congestion — frames that never arrived,
//          frames decoded but never shown, utilisation at the ceiling — or on the slowdown
//          above, the maximum filter is first capped to what the link is measurably doing
//          right now (its ten second memory has just been proven stale by an event rather
//          than aged out), and the bitrate goes to 0.7 times that. The state returns to
//          steady, whose 0.85 gain brings the bitrate back up one second later: the
//          undershoot is BBR's DRAIN, there to empty the queue that built up, not a new
//          operating point.
// A degrading radio trend forces steady with gain 0.75 and blocks probing, reusing the same
// radio_hold flag v1 uses to block its additive increase.
class bitrate_controller
{
public:
	using clock = std::chrono::steady_clock;
	using mode = wivrn::bitrate_mode;

	struct config
	{
		// NX default is on
		bool enabled = true;
		// Never go below this, whatever the measurements say
		uint32_t min_bitrate_bps = 10'000'000;
		// Control law used when the headset expresses no preference of its own. The
		// headset's selector always wins over this.
		mode control = mode::aimd;
	};

	// --- Measurement window -------------------------------------------------------------
	// Duration of the sliding window of per-frame samples.
	static constexpr std::chrono::milliseconds window_duration{2000};
	// No decision is taken with fewer samples than this in the window.
	static constexpr size_t min_samples = 20;
	// Upper bound on how often the policy is evaluated.
	static constexpr std::chrono::milliseconds evaluation_interval{250};
	// Percentile of the utilisation samples used for every threshold comparison.
	static constexpr double utilisation_percentile = 0.9;
	// Number of frames tracked simultaneously; a frame is turned into a sample once this many
	// newer frames have been seen, which is enough for the late "displayed" feedback to arrive.
	static constexpr size_t frame_ring_size = 16;
	// One video stream per encoder: left, right, alpha. Anything else is not a video frame.
	//
	// Deliberately not the promoted quad layer stream, which is one index further:
	// it is silent on every frame that promotes no layer, and the headset reports the
	// frames it never received as lost, which here would read as a saturated link.
	static constexpr uint8_t video_stream_count = 3;

	// --- Thresholds ---------------------------------------------------------------------
	// p90 utilisation above this: the link is saturated, decrease.
	static constexpr double utilisation_decrease = 0.85;
	// p90 utilisation below this: there is spare capacity, probe upwards.
	// The gap with utilisation_decrease is the hysteresis band, and is mandatory: inside it
	// nothing happens at all.
	static constexpr double utilisation_increase = 0.60;
	// p90 utilisation above this: acute congestion, a frame no longer fits in a frame period.
	static constexpr double utilisation_severe = 1.00;
	// Frames that never arrived completely, counted over the window.
	static constexpr size_t lost_frames_decrease = 1;
	static constexpr size_t lost_frames_severe = 3;
	// Frames decoded but dropped before being displayed, counted over the window.
	static constexpr size_t late_frames_decrease = 4;
	static constexpr size_t late_frames_severe = 10;

	// --- Decrease -----------------------------------------------------------------------
	// Gentle multiplicative decrease, for gradual degradation.
	static constexpr double decrease_factor = 0.9;
	// Acute congestion uses a bounded cut; persistent trouble repeats it using fresh samples.
	static constexpr double deep_decrease_factor = 0.8;
	static constexpr std::chrono::milliseconds aimd_decrease_cooldown{500};
	// Existing cooldown for bandwidth-estimator and radio-triggered decreases.
	static constexpr std::chrono::milliseconds decrease_cooldown{2000};

	// --- Slow additive increase (above the recovery target) -----------------------------
	// Increase step, whichever is larger.
	static constexpr uint32_t increase_step_min = 2'000'000;
	static constexpr double increase_step_ratio = 0.02; // of the ceiling
	// The link must measure healthy for this long before every increase. As the window is
	// flushed on every change this also acts as the increase cooldown.
	static constexpr std::chrono::milliseconds increase_hold{1000};

	// --- Fast recovery (after a deep drop, up to the pre-drop bitrate) ------------------
	// Healthy period required before the first rebound step.
	static constexpr std::chrono::milliseconds recovery_confirm{1000};
	// Healthy period required before each subsequent rebound step.
	static constexpr std::chrono::milliseconds recovery_step_interval{500};
	// Smaller multiplicative steps avoid a doubling spike during recovery.
	static constexpr double recovery_factor = 1.15;
	// If congestion comes back while rebounding, lower the rebound target by this factor.
	static constexpr double recovery_target_backoff = 0.75;

	// --- Radio trend (preemptive decrease) ----------------------------------------------
	// Length of the trend window. Long enough that the ±3 dB the radio reports while the
	// user stands still averages out, short enough to still be ahead of the loss.
	static constexpr std::chrono::milliseconds radio_trend_window{4000};
	// A sample older than this is stale: the headset stopped reporting (an older client, a
	// non-Android one, a failed read), and a trend from before the gap says nothing about
	// the link now. Stale data is ignored entirely and releases any hold.
	static constexpr std::chrono::milliseconds radio_max_age{5000};
	// No trend is computed from fewer samples, i.e. not before ~3 s of reports at 1 Hz.
	static constexpr size_t radio_min_samples = 4;
	// Smoothing of the reported RSSI before it is used as "the current level". One sample
	// per second, so this is a time constant of roughly two seconds.
	static constexpr double radio_ema_alpha = 0.4;
	// The fall, in dB over the window, that separates walking away from the access point
	// from the noise of standing still. Read off the least squares slope times the time the
	// window actually covers.
	static constexpr double radio_fall_db = 6.0;
	// ... but a fall only matters once the absolute level is low enough that the radio's
	// rate adaptation is about to start dropping MCS. -65 dBm is roughly where a 5 GHz link
	// leaves its top rates; above it there is margin for the fall to eat.
	static constexpr double radio_low_rssi_dbm = -65.0;
	// Second, independent trigger: the radio's own rate adaptation already halved the PHY
	// rate compared to the best it saw in the window, *and* what is left is less than this
	// multiple of the bitrate being sent. PHY rates are nominal — aggregation, contention
	// and the uplink all take their share — so twice the video bitrate is already the edge.
	static constexpr double radio_link_speed_headroom = 2.0;
	static constexpr double radio_link_speed_collapse = 0.5;
	// Minimum time between two preemptive steps, on top of decrease_cooldown: a preemptive
	// step is a guess, and the frame timings must be given time to confirm or deny it.
	static constexpr std::chrono::milliseconds radio_step_interval{4000};
	// The signal coming back up by this much over the window releases the hold that a
	// preemptive step put on the normal probing-upwards path.
	static constexpr double radio_rise_db = 3.0;
	// A hold is also released when the signal simply stops falling: the user walked to a
	// new spot and settled at a lower but steady level, with reports still coming and
	// nothing congested. The rise test above never fires for that (there is no rise
	// back), so the hold would otherwise latch forever and block every upward probe. The
	// slope, in dB/s, below which |slope| counts as "stabilised, no longer falling". Well
	// under the ~1.5 dB/s slope a genuine radio_fall_db fall shows over the trend window.
	static constexpr double radio_stable_slope = 0.75;
	// How long the slope has to stay flat before that release fires: long enough that the
	// trend window has refilled with post-move samples and a momentary flat patch in an
	// ongoing fall does not trip it.
	static constexpr std::chrono::milliseconds radio_stable_hold{3000};

	// --- v2: delivered-bandwidth estimator ----------------------------------------------
	// Memory of the maximum filter on the delivery rate. BBR uses ten round trips for its
	// bandwidth filter and a ten second window for min_rtt; ten seconds is long enough to
	// cover a few probe cycles and short enough that capacity lost for good (a wall between
	// the headset and the access point) is forgotten while the user is still in the room.
	static constexpr std::chrono::milliseconds estimator_window{10000};
	// A delivery rate sample only counts towards the maximum filter when the frame kept the
	// link busy for at least this fraction of the *paced* window. Below it the frame is one
	// micro-burst and the measurement is quantisation noise on a stopwatch, not a capacity.
	// See the app-limited discussion above.
	static constexpr double app_limited_wire_fraction = 0.30;
	// The same idea with pacing switched off, where there is no window to be a fraction of:
	// a frame must have occupied at least this much of a frame period.
	static constexpr double unpaced_wire_fraction = 0.10;
	// No bitrate is derived from the estimate before this many delivery rate samples have
	// been admitted, i.e. not before a couple hundred milliseconds of loaded video. A
	// maximum filter is exactly the wrong thing to trust after one noisy sample.
	static constexpr size_t estimator_min_samples = 12;

	// Gains applied to the bandwidth estimate, one per state.
	static constexpr double gain_startup = 1.25;
	static constexpr double gain_steady = 0.85;
	static constexpr double gain_probe = 1.10;
	// Steady gain while the radio trend says the link is on its way down. Lower than
	// gain_steady on purpose: the estimate is about to be wrong and in the optimistic
	// direction, and the radio is the only thing that knows it yet.
	static constexpr double gain_radio = 0.75;
	// Acute congestion: the maximum filter is first capped to what the link is measured to
	// be delivering right now, and the bitrate goes to this times that. Below gain_steady on
	// purpose — the gap between the two is a drain, not a new operating point, and the next
	// steady evaluation a second later takes the bitrate back up to the steady gain.
	static constexpr double backoff_factor = 0.70;

	// One "round" of the startup ramp. Not a round trip — there is no RTT here — but the
	// interval over which the estimate is asked whether it is still growing.
	static constexpr std::chrono::milliseconds round_duration{500};
	// Growth below this factor over a round counts as no growth at all.
	static constexpr double startup_growth = 1.25;
	// Consecutive rounds without growth that end the startup ramp. Three is BBR's number:
	// one flat round is noise, three in a row is a plateau.
	static constexpr size_t startup_stall_rounds = 3;

	// Time between two probes, and how long one lasts. One round is enough to see whether
	// the frames sent at the raised gain came back faster; longer only spends more time
	// overshooting a link that is already the bottleneck.
	static constexpr std::chrono::milliseconds probe_interval{8000};
	static constexpr std::chrono::milliseconds probe_duration{500};

	// In steady state the bitrate is not allowed to move more often than this, nor at all
	// unless the new target differs from the current bitrate by more than the threshold.
	// Re-encoding at a new bitrate is not free and the estimate wobbles by a few percent
	// frame to frame; without both of these the controller would chase that wobble.
	static constexpr std::chrono::milliseconds steady_interval{1000};
	static constexpr double steady_change_threshold = 0.05;
	// Nothing is evaluated for this long after a change of bitrate. The frames that were
	// already in flight when it happened are dropped outright (see flush), but the encoder
	// still needs a frame or two to actually reach the new rate, and the short window has to
	// refill before a percentile over it means anything.
	static constexpr std::chrono::milliseconds change_settle{250};

	// How much slower than the ten second maximum the link has to be handing frames over
	// before that counts as congestion rather than as noise. Both sides of the ratio are high
	// percentiles, so this is not one unlucky frame; a third of the capacity gone is a real
	// change in the link.
	static constexpr double slowdown_backoff = 1.60;

	// --- Emergency half-rate (last resort, below the bitrate floor) ----------------------
	// The rung below everything else: when the bitrate is already pinned at the floor and
	// the link is still severely congested, halving the stream framerate instantly halves
	// the bandwidth. Detected here from the same window; applied by the session, which owns
	// the compositor framerate and the "server config AND headset toggle" gate.
	//
	// Sustained severe congestion while pinned at the floor for this long engages it.
	static constexpr std::chrono::milliseconds emergency_trigger_duration{3000};
	// The link has to stay clean (healthy, no loss, no late frames) for this long before the
	// full framerate is restored: a hysteresis window so a single good patch does not undo it.
	static constexpr std::chrono::milliseconds emergency_clear_duration{5000};
	// Neither engaging nor restoring may happen within this of the last transition, so the
	// mode cannot oscillate on a link hovering at the edge.
	static constexpr std::chrono::milliseconds emergency_min_dwell{4000};

	bitrate_controller() = default;

	// Set the configuration and the initial ceiling. The ceiling is the bitrate the client asked
	// for; the controller never goes above it. client_enabled is the headset side switch: the
	// control only runs when both it and the server configuration are enabled. radio_aware is
	// the headset side switch for the preemptive radio trend, which additionally requires the
	// automatic bitrate to be on at all. client_mode is the control law the headset asked for,
	// empty when it asked for none and config::control decides.
	void configure(const config &, uint32_t ceiling_bps, bool client_enabled, bool radio_aware, std::optional<mode> client_mode = {});

	bool enabled() const;
	uint32_t current() const;

	// The headset toggled its own switch. Starts over, as measurements taken under the other
	// setting say nothing; switching off therefore restores the full ceiling. Returns the
	// bitrate to apply, if any.
	std::optional<uint32_t> set_client_enabled(bool);

	// The headset toggled the radio trend switch. Never changes the bitrate on its own:
	// switching off only stops the preemptive steps and releases any hold they left, the
	// normal AIMD takes it from there.
	void set_radio_aware(bool);

	// The control law the headset asked for, empty when it expresses no preference and the
	// server configuration decides. Measurements taken under the other law say nothing about
	// this one — the two do not even measure the same quantity — so a change starts over,
	// exactly like the headset switch. Returns the bitrate to apply, if any.
	std::optional<uint32_t> set_client_mode(std::optional<mode>);

	// Control law actually in force.
	mode active_mode() const;

	// Everything the headset's Transport page shows about the bitrate control, read in one
	// go under the controller's own mutex so that the fields agree with each other rather
	// than being stitched together from separate accessors racing the evaluator.
	struct status
	{
		uint32_t bitrate_bps = 0;
		uint32_t ceiling_bps = 0;
		mode control = mode::aimd;
		to_headset::transport_status::controller_state state =
		        to_headset::transport_status::controller_state::off;
		bool radio_hold = false;
		// Emergency half-rate mode currently engaged (the stream framerate is halved).
		bool emergency = false;
	};
	status snapshot() const;

	// The "server config AND headset toggle" gate for the emergency half-rate mode. When it
	// goes false the mode is released at once. Detection only runs while it is true.
	void set_emergency_enabled(bool enabled);

	// Whether the emergency half-rate mode is currently engaged. This is the source of truth
	// the session mirrors onto the compositor (idempotently), so a change reached by any path
	// — a normal transition, the gate flipping, or a reset that forgot the measurements — is
	// always reflected. Also the HUD state.
	bool emergency_framerate_active() const;

	// The pacing window currently in force, as a fraction of a frame period, or 0 when the
	// video shards are not paced. Only the v2 estimator uses it, to tell a frame that filled
	// its paced window from one that was over in a single micro-burst.
	void set_pacing_window(float window);

	// Bytes the server put on the wire for one video frame of one stream, parity shards
	// included: the unit the bitrate itself is expressed in. Called from the encoder's send
	// path, i.e. from a different thread than on_feedback and usually a frame or two ahead
	// of it; the frame ring joins the two. Allocation-free, and a no-op unless the v2
	// estimator is the one running.
	// Optional direct-frame metadata freezes the applied quality budget and cadence;
	// ordinary encoders leave both zero. These fields never go onto the wire.
	void on_frame_bytes(uint64_t frame_index, uint8_t stream_index, uint32_t bytes, clock::time_point now = clock::now(), uint32_t quality_budget_bps = 0, int64_t quality_period_ns = 0);

	// A new ceiling was requested (client settings, or a manual change from the dashboard).
	// Resets the controller to it. Returns the bitrate to apply.
	std::optional<uint32_t> set_ceiling(uint32_t ceiling_bps);

	// Extra ceiling imposed by the path currently carrying video (multipath failover), clamped
	// on top of the client's. Empty puts the client ceiling back. Measurements taken on the
	// other path say nothing about this one, so the controller is re-seeded either way. Returns
	// the bitrate to apply — including when the automatic control is off, the path budget is a
	// property of the link and not of the AIMD.
	std::optional<uint32_t> set_path_ceiling(std::optional<uint32_t> ceiling_bps);

	// Multipath stage 3: video is striped over both paths at once, or is not any more.
	//
	// --- What this does, and deliberately does not, do to the accounting ---------------
	// It does *not* raise the ceiling. The ceiling is the bitrate the headset asked for and
	// it stays exactly that: aggregating two links is a way to *reach* the number the user
	// chose over a Wi-Fi link that could not, not a licence to go past it. usb_bps is taken
	// for the record (and for the log line) rather than added to anything.
	//
	// It does *not* credit the estimator with headroom either, and this is the important
	// half. The delivery rate the v2 estimator measures is 8 * frame_bytes / (received_last
	// - received_first), and both timestamps are the client's own: while combining, the
	// bytes are the whole frame's, on both paths, and the span runs from the first arrival
	// on either path to the last on either path. That is already an aggregate measurement —
	// the frame really did cross both links inside that span — so the estimator needs no
	// help to discover the extra capacity, and crediting it would double count.
	//
	// What it *does* do is make sure the failover clamp is out of the way (see
	// effective_ceiling: the USB path budget applies to a session running *on* the USB
	// path, not to one merely spilling its tail onto it), and re-seed everything, because
	// the link the controller was measuring is not the link it is about to measure. That
	// re-seed is what also makes a collapse out of combining correct: the caller applies the
	// single-path ceiling in the same breath, and the estimate that was describing two links
	// is thrown away instead of driving the bitrate of one.
	//
	// Returns the bitrate to apply, if any.
	std::optional<uint32_t> set_combined(bool combined, uint32_t usb_bps);

	// The delivered-bandwidth estimate in bits per second, or 0 when the v2 estimator is not
	// the law in force or has not admitted enough samples yet. While video rides a single
	// path this is a measurement of *that* path, which is what the striping scheduler latches
	// as its Wi-Fi share when it enters the combine posture.
	uint32_t bandwidth_estimate() const;

	// Forget all measurements and go back to the ceiling, e.g. when the session is resumed.
	std::optional<uint32_t> reset();

	// Feed one feedback packet. frame_period_ns is the current video frame period (from the
	// pacer), streaming tells whether the stream is up at all. Returns a new bitrate to apply,
	// if any. now is injectable so the policy can be driven on a virtual clock by the tests;
	// leaving it out is the real thing.
	std::optional<uint32_t> on_feedback(const from_headset::feedback &, int64_t frame_period_ns, bool streaming, clock::time_point now = clock::now());

	// Feed one Wi-Fi radio report from the headset. Only call it for a sample the headset
	// marked valid; obviously impossible values (a non-negative or absurdly low RSSI, the
	// -127 sentinel) are rejected here as well. link_speed_mbps <= 0 means unknown, the rest
	// of the sample is still used. Returns a lower bitrate to apply, if the trend calls for a
	// preemptive step; never a higher one.
	std::optional<uint32_t> on_wifi_state(int rssi_dbm, int link_speed_mbps, clock::time_point when = clock::now());

private:
	// State of one video frame while its per-stream feedback packets are being collected.
	struct frame_state
	{
		uint64_t index = uint64_t(-1);
		XrTime first = 0; // earliest received_first_packet over all streams
		XrTime last = 0;  // latest received_last_packet over all streams
		// The same pair kept PER STREAM, and what the utilisation is actually
		// measured from.
		//
		// The merged pair above is only a wire time when every stream's frame `index`
		// names the same composited frame. On the shard codecs it does. On nxwarp it
		// does not: that codec numbers its feedback from a per-stream WIRE id and the
		// two eyes are paced independently, so their ids drift -- live, stream 0 sent
		// 76 frames in the same two seconds stream 1 sent 67. `last - first` then
		// measured the drift between two unrelated frames, p90 utilisation read 3.27
		// and then 21.08 against a threshold of 1.00, and "acute congestion" halved
		// the bitrate twice with ZERO lost datagrams.
		//
		// A frame's time on the wire is a property of one stream, so it is measured
		// in one stream and the widest is taken. That is true for every codec and
		// needs nothing on the wire.
		static constexpr size_t max_streams = 4;
		std::array<XrTime, max_streams> stream_first{};
		std::array<XrTime, max_streams> stream_last{};
		bool valid = false;
		bool lost = false; // at least one stream never arrived completely
		bool late = false; // decoded but dropped before being displayed
		// NX transport bytes sent for this frame (outer framing/tail packets excluded),
		// summed over the video streams. Filled in
		// from the encoder's send path, v2 only.
		uint64_t bytes = 0;
		uint32_t quality_budget_bps = 0; // Controller input applied to this direct frame.
		int64_t quality_period_ns = 0;
	};

	struct sample
	{
		clock::time_point when;
		float utilisation = 0;
		bool lost = false;
		bool late = false;
		// Delivery rate this frame measured, bits per second, or 0 when it says nothing
		// about the capacity (v1, a lost frame, an app-limited one)
		double rate = 0;
		double quality_scale = 0; // Quality bps / offered wire bps for this frame.
	};

	// Windowed maximum of a scalar, Kathleen Nichols' three-sample filter, the same one BBR
	// uses for its bandwidth estimate. Keeps the running maximum plus the best of the last
	// third and the last sixth of the window, so that when the oldest one ages out there is
	// already a valid replacement covering the recent past. O(1) per sample, no allocation,
	// three entries whatever the frame rate.
	class max_filter
	{
	public:
		bool valid() const
		{
			return count != 0;
		}
		// Meaningless when not valid()
		double get() const
		{
			return s[0].value;
		}

		void reset()
		{
			count = 0;
			s = {};
		}

		void update(double value, clock::time_point now, clock::duration window)
		{
			++count;

			// First sample, a new extremum, or everything on record is stale.
			if (count == 1 or better(value, s[0].value) or now - s[2].when > window)
			{
				s[0] = s[1] = s[2] = {now, value};
				return;
			}

			if (better(value, s[1].value))
				s[1] = s[2] = {now, value};
			else if (better(value, s[2].value))
				s[2] = {now, value};

			if (now - s[0].when > window)
			{
				// The running extremum aged out: promote the two sub-window ones.
				s[0] = s[1];
				s[1] = s[2];
				s[2] = {now, value};
				if (now - s[0].when > window)
				{
					s[0] = s[1];
					s[1] = s[2];
				}
			}
			else if (s[1].when == s[0].when and now - s[1].when > window / 4)
				s[1] = s[2] = {now, value};
			else if (s[2].when == s[1].when and now - s[2].when > window / 2)
				s[2] = {now, value};
		}

		// Force the maximum down to at most `value`: what the filter remembers has just
		// been proven wrong by an event rather than aged out.
		void cap(double value)
		{
			for (auto & e: s)
			{
				if (better(e.value, value))
					e.value = value;
			}
		}

	private:
		struct entry
		{
			clock::time_point when{};
			double value = 0;
		};

		static bool better(double a, double b)
		{
			return a >= b;
		}

		std::array<entry, 3> s{};
		size_t count = 0;
	};

	// One Wi-Fi report, after the sentinel filtering.
	struct radio_sample
	{
		clock::time_point when;
		double rssi_dbm = 0; // smoothed, not the raw report
		int link_speed_mbps = 0;
	};

	// What the window says about the radio right now.
	struct radio_trend
	{
		bool usable = false;
		double rssi_dbm = 0; // smoothed current level
		double slope_db_per_s = 0;
		double span_s = 0;
		// Change over the window, positive while the signal improves
		double delta_db = 0;
		int link_speed_mbps = 0;
		int link_speed_peak_mbps = 0;
	};

	enum class state
	{
		// Slow AIMD around the current bitrate.
		steady,
		// Below a remembered pre-drop bitrate after a deep drop, rebounding fast.
		recovering,
	};

	// v2 only, see the state machine described at the top of this file.
	enum class bbr_state
	{
		startup,
		steady,
		probe,
	};

	struct stats
	{
		double utilisation = 0; // high percentile over the window
		size_t lost = 0;
		size_t late = 0;
		size_t count = 0;
		// High percentile of the delivery rates in the window, and how many there were.
		// The best the link has recently been seen doing, against which the ten second
		// maximum is compared; v2 only.
		double rate = 0;
		size_t rate_count = 0;
		double quality_scale = 0;
	};

	mutable std::mutex mutex;

	config conf;
	// Headset side switch, ANDed with conf.enabled
	bool client_enabled = true;
	// Control law the headset asked for; conf.control when it asked for none
	std::optional<mode> client_mode;
	// Headset side switch for the radio trend, ANDed with the two above
	bool radio_aware = true;
	// Diagnostic only: ignore utilisation-only AIMD decreases while retaining loss/late cuts.
	bool aimd_loss_only = false;
	double aimd_probe_ratio = 0.35;
	// Mapping is supported only for a sole primary direct stereo stream.
	uint8_t quality_stream_mask = 0;
	uint8_t measured_stream_mask = 0;
	// Ceiling requested by the client
	uint32_t ceiling = 0;
	// Ceiling of the path carrying video, if it is more restrictive
	std::optional<uint32_t> path_ceiling;
	// Video is striped over both paths (stage 3): the path ceiling, which exists to cap a
	// session running on the USB tunnel alone, does not apply
	bool combined = false;
	// The USB path's configured budget while combining. Recorded, not spent: see set_combined
	uint32_t combined_usb_bps = 0;
	uint32_t bitrate = 0;
	uint32_t min_bitrate = 0;

	state st = state::steady;
	uint32_t recovery_target = 0;
	bool first_recovery_step = true;

	std::array<frame_state, frame_ring_size> frames;
	uint64_t newest_frame = 0;
	bool has_frames = false;
	// Frames older than this were sent at a bitrate that is no longer in force, and say
	// nothing about the one that is. Moved past everything in flight by every flush()
	uint64_t stale_before = 0;
	// Current video frame period in ns, from the pacer
	int64_t frame_period = 0;
	// Fraction of a frame period the video shards are spread over, 0 when not paced
	float pacing_window = 0;

	// --- v2 estimator state -------------------------------------------------------------
	bbr_state bbr_st = bbr_state::startup;
	// Bottleneck bandwidth estimate, bits per second, maximum over estimator_window
	max_filter bandwidth;
	// Delivery rate samples admitted into it, and when the last one was: an estimate no
	// loaded frame has refreshed for a whole window is not evidence of a bottleneck any more
	size_t bandwidth_samples = 0;
	clock::time_point last_bandwidth_sample{};
	// Value the bandwidth estimate had at the start of the current startup round, and how
	// many consecutive rounds it has failed to grow
	double startup_mark = 0;
	size_t startup_stalled = 0;
	clock::time_point round_started{};
	// Smaller direct-stream retries after congestion; clean probes restore the gain.
	double direct_probe_gain = gain_probe;
	clock::time_point last_probe{};
	clock::time_point probe_until{};
	clock::time_point last_bbr_change{};

	std::deque<sample> window;
	std::optional<clock::time_point> healthy_since;
	clock::time_point last_evaluation{};
	clock::time_point last_decrease{};

	std::deque<radio_sample> radio_window;
	// Smoothed RSSI carried across samples, empty until the first one
	std::optional<double> radio_ema;
	clock::time_point last_radio_sample{};
	clock::time_point last_radio_step{};
	// A preemptive step was taken and the radio has not recovered since: hold the normal
	// probing upwards, it would only walk straight back into the degradation. Released by a
	// recovering signal, by the signal stabilising at a lower level (see radio_stable_slope),
	// by the radio data going stale, and by every reset.
	bool radio_hold = false;
	// When the slope first read flat while a hold was in force, for the stabilisation
	// release. Empty whenever the signal is falling, starved, or not being held.
	std::optional<clock::time_point> radio_stable_since;

	// --- Emergency half-rate state ------------------------------------------------------
	// The gate (server config AND headset toggle), set by the session. Detection runs only
	// while true.
	bool emergency_enabled = false;
	// Whether the mode is engaged right now.
	bool emergency_active = false;
	// When the pinned-at-floor severe condition began (engage timer), and when the clean
	// condition began (restore timer). Default-constructed means "not currently counting".
	clock::time_point emergency_severe_since{};
	clock::time_point emergency_clean_since{};
	// When the mode last flipped, for the anti-oscillation dwell.
	clock::time_point emergency_last_change{};

	// Ceiling actually in force: the client's, clamped by the current path's
	uint32_t effective_ceiling() const;
	// active_mode(), with the mutex already held
	mode mode_locked() const;
	// reset(), with the mutex already held
	std::optional<uint32_t> reset_locked();
	void close_frame(frame_state &, clock::time_point now);
	void flush();
	// Forget the radio trend and release the hold
	void flush_radio();
	// Forget everything the v2 estimator learned
	void flush_estimator();
	// Shortest wire span, in ns, that is not an app-limited measurement
	int64_t min_loaded_wire_ns() const;
	stats analyse(clock::time_point now);
	radio_trend analyse_radio(clock::time_point now);
	std::optional<uint32_t> evaluate(clock::time_point now);
	std::optional<uint32_t> evaluate_aimd(clock::time_point now, const stats &);
	std::optional<uint32_t> evaluate_bbr(clock::time_point now, const stats &);
	// Drive the emergency half-rate state machine off one analysed window. Mutex held.
	void update_emergency(clock::time_point now, const stats &);
	uint32_t clamp(uint64_t value) const;
};

} // namespace wivrn
