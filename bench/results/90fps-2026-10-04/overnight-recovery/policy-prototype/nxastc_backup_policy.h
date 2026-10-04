#pragma once

#include <cstdint>
#include <optional>

namespace wivrn::nxastc_backup
{
// display_target_ns is the server-predicted display target, not capture time.
// It is suitable for pair agreement and presentation ordering only; it does not
// measure image age or capture-to-display latency. Decode times use the client clock.
struct pair
{
	uint64_t left_frame = 0, right_frame = 0;
	uint64_t left_target_ns = 0, right_target_ns = 0;
	uint64_t left_decoded_ns = 0, right_decoded_ns = 0;

	bool coherent() const
	{
		return left_frame == right_frame && left_target_ns == right_target_ns;
	}
	uint64_t frame() const { return left_frame; }
	uint64_t target_ns() const { return left_target_ns; }
};

enum class choice { hold, primary, backup };

class policy
{
public:
	choice select(uint64_t now_ns, uint64_t period_ns,
	              std::optional<pair> primary, std::optional<pair> backup,
	              bool focused = true)
	{
		if (last_now_ns && now_ns < *last_now_ns)
		{
			reset();
			last_now_ns = now_ns;
			return choice::hold;
		}
		last_now_ns = now_ns;
		if (!focused)
		{
			reset();
			return choice::hold;
		}
		if (!period_ns)
			return choice::hold;

		if (primary && primary->coherent() && newer_than_selected(*primary) &&
		    (!last_primary_frame || primary->frame() != *last_primary_frame) &&
		    age_at_most(primary->left_decoded_ns, now_ns, period_ns) &&
		    age_at_most(primary->right_decoded_ns, now_ns, period_ns))
		{
			last_primary_frame = primary->frame();
			last_primary_selected_at_ns = now_ns;
			last_selected_target_ns = primary->target_ns();
			missing_since_ns.reset();
			return choice::primary;
		}
		if (!missing_since_ns)
			missing_since_ns = last_primary_selected_at_ns.value_or(now_ns);
		if (now_ns < *missing_since_ns || !at_least_two_periods(now_ns - *missing_since_ns, period_ns))
			return choice::hold;

		if (!backup || !backup->coherent() || !newer_than_selected(*backup) ||
		    !age_at_most(backup->left_decoded_ns, now_ns, period_ns, true) ||
		    !age_at_most(backup->right_decoded_ns, now_ns, period_ns, true))
			return choice::hold;
		last_selected_target_ns = backup->target_ns();
		return choice::backup;
	}

	void reset()
	{
		last_now_ns.reset();
		last_primary_frame.reset();
		last_primary_selected_at_ns.reset();
		last_selected_target_ns.reset();
		missing_since_ns.reset();
	}

private:
	static bool at_least_two_periods(uint64_t age, uint64_t period)
	{
		return age >= period && age - period >= period;
	}
	static bool age_at_most(uint64_t decoded_ns, uint64_t now_ns, uint64_t period_ns, bool twice = false)
	{
		if (decoded_ns > now_ns)
			return false;
		const auto age = now_ns - decoded_ns;
		return twice ? (age <= period_ns || age - period_ns <= period_ns) : age <= period_ns;
	}
	bool newer_than_selected(const pair & candidate) const
	{
		return !last_selected_target_ns || candidate.target_ns() > *last_selected_target_ns;
	}

	std::optional<uint64_t> last_now_ns;
	std::optional<uint64_t> last_primary_frame;
	std::optional<uint64_t> last_primary_selected_at_ns;
	std::optional<uint64_t> last_selected_target_ns;
	std::optional<uint64_t> missing_since_ns;
};
} // namespace wivrn::nxastc_backup
