#define main existing_test_main
#include "reference-bbr-harness.cpp"
#undef main

int main(int argc, char **argv)
{
	if (argc != 3) return 2;
	FILE *rows = std::fopen(argv[1], "w");
	FILE *summary = std::fopen(argv[2], "w");
	if (!rows || !summary) return 2;
	std::fprintf(rows, "burst_offset,frame,burst,bitrate_bps,estimate_bps\n");
	std::fprintf(summary, "burst_offset,quiet_target_bps,phase_min_bps,phase_max_bps,final_bps,frames_above_quiet,frames_below_quiet,first_excursion_frame,return_to_quiet_frame\n");
	for (int burst = -1; burst < 47; ++burst)
	{
		harness h(mode::bbr, 24e6);
		h.quiet();
		h.feed(360); // four virtual seconds after quiet, to align with later probe epochs
		const uint32_t quiet_target = h.current();
		uint32_t low = quiet_target, high = quiet_target;
		int frames_above = 0, frames_below = 0;
		int first_excursion = -1, return_to_quiet = -1;
		bool left_quiet = false;
		for (int i = 0; i < 270; ++i)
		{
			const auto bytes = h.frame_bytes();
			const auto wire = i == burst ? std::max<int64_t>(period * 0.4, int64_t(8e9 * double(bytes) / 48e6)) : h.wire_ns(bytes);
			h.feed_raw(bytes, wire);
			const auto target = h.current();
			low = std::min(low, target);
			high = std::max(high, target);
			if (target > quiet_target) ++frames_above;
			if (target < quiet_target) ++frames_below;
			if (target != quiet_target)
			{
				left_quiet = true;
				if (first_excursion < 0) first_excursion = i;
			}
			else if (left_quiet && return_to_quiet < 0)
				return_to_quiet = i;
			std::fprintf(rows, "%d,%d,%d,%u,%u\n", burst, i, i == burst ? 1 : 0, target, h.ctl.bandwidth_estimate());
		}
		std::fprintf(summary, "%d,%u,%u,%u,%u,%d,%d,%d,%d\n", burst, quiet_target, low, high, h.current(), frames_above, frames_below, first_excursion, return_to_quiet);
		std::printf("burst=%d quiet=%u min=%u max=%u final=%u above=%d below=%d first=%d return=%d\n", burst, quiet_target, low, high, h.current(), frames_above, frames_below, first_excursion, return_to_quiet);
	}
	const bool ok = std::fclose(rows) == 0 && std::fclose(summary) == 0;
	return ok ? 0 : 2;
}
