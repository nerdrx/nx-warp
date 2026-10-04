/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace wivrn
{
inline constexpr int64_t nack_quiet_period_ns = 2'500'000;
inline constexpr uint8_t nack_max_rounds = 2;

inline bool nack_quiet_elapsed(int64_t now, int64_t since)
{
	return since > 0 and now >= since and uint64_t(now) - uint64_t(since) >= uint64_t(nack_quiet_period_ns);
}

// Returns the next quiet-gate deadline. The exact missing-shard scan runs only once the
// candidate is due; before then it is deferred so active shard arrivals do not trigger it.
template <typename HasMissing>
std::optional<int64_t> nack_poll_deadline(
        int64_t now,
        int64_t last_shard,
        int64_t nack_last,
        uint8_t rounds,
        bool enabled,
        bool send_available,
        bool nonempty,
        bool complete,
        HasMissing && has_missing_when_due)
{
	if (not enabled or not send_available or not nonempty or complete or rounds >= nack_max_rounds)
		return {};

	const int64_t since = std::max(last_shard, nack_last);
	if (since <= 0 or now < since or since > std::numeric_limits<int64_t>::max() - nack_quiet_period_ns)
		return {};

	const int64_t due = since + nack_quiet_period_ns;
	if (now >= due and not has_missing_when_due())
		return {};
	return due;
}

inline std::chrono::milliseconds nack_poll_timeout(
        int64_t now,
        std::optional<int64_t> due,
        std::chrono::milliseconds max_wait)
{
	if (not due or max_wait.count() <= 0)
		return max_wait;
	if (now >= *due)
		return std::chrono::milliseconds{1};

	// Both times are positive and ordered (guaranteed by nack_poll_deadline).
	const uint64_t remaining = uint64_t(*due) - uint64_t(now);
	const uint64_t ms = remaining / 1'000'000 + (remaining % 1'000'000 != 0);
	return std::chrono::milliseconds{std::clamp<uint64_t>(ms, 1, std::min<uint64_t>(100, uint64_t(max_wait.count())))};
}
} // namespace wivrn
