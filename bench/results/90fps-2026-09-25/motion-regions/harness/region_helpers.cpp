#define main archived_direct_motion_test_main
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#endif
#include "direct_motion_test.cpp"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef main

#include "common/nxwarp_direct_motion_regions.h"

static layout regions_layout()
{
	auto l = motion_layout();
	l.motion_regions = true;
	return l;
}

static unsigned scalar_region_index(layout l, int x, int y)
{
	return unsigned(x >= int(l.width / 64 * 32)) + 2u * unsigned(y >= int(l.height / 64 * 32));
}

static std::vector<uint8_t> make_aligned_regions_base(layout l)
{
	const uint32_t cols = l.width / 32, rows = l.height / 32, tiles = l.tile_count();
	const uint32_t ox = ((l.width - l.native_side) / 2) & ~31u;
	const uint32_t oy = ((l.height - l.native_side) / 2) & ~31u;
	const uint32_t native_tiles = (l.native_side / 32) * (l.native_side / 32) * l.eyes;
	const uint32_t words = native_tiles * native_rgb_words;
	auto raw = frame_header(tiles, words, native_version);
	uint32_t offset = 0;
	for (uint32_t y = 0; y < rows; ++y)
		for (uint32_t eye = 0; eye < l.eyes; ++eye)
			for (uint32_t x = 0; x < cols; ++x)
			{
				const bool native = x * 32 >= ox && x * 32 < ox + l.native_side &&
				                    y * 32 >= oy && y * 32 < oy + l.native_side;
				append32(raw, native ? 0x20000000u | offset : 0xc0112233u);
				if (native) offset += native_rgb_words;
			}
	const size_t payload = 16 + size_t(tiles) * 4;
	raw.resize(payload + size_t(words) * 4, 0);
	for (uint32_t y = 0; y < rows; ++y)
		for (uint32_t eye = 0; eye < l.eyes; ++eye)
			for (uint32_t x = 0; x < cols; ++x)
			{
				const uint32_t d = read32(raw, 16 + (size_t(y) * cols * l.eyes + eye * cols + x) * 4);
				if (!(d & 0x20000000u)) continue;
				const size_t base = payload + size_t(d & 0x0fffffffu) * 4;
				for (int py = 0; py < 32; ++py)
					for (int px = 0; px < 32; ++px)
						for (unsigned c = 0; c < 4; ++c)
							raw[base + (size_t(py) * 32 + px) * 4 + c] = pattern(eye, int(x * 32) + px, int(y * 32) + py, c);
			}
	return raw;
}

static std::vector<uint8_t> make_regions_frame(layout l, const std::vector<uint8_t> & old,
		const motion_native_info & info, const motion_region_vectors & vectors)
{
	auto now = old;
	const int x0 = int((l.width - l.native_side) / 2) & ~31, y0 = int((l.height - l.native_side) / 2) & ~31;
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile)
	{
		const int32_t dst = info.pixel[tile];
		if (dst < 0) continue;
		const int ty = int(tile / (l.width / 32 * l.eyes)), rem = int(tile % (l.width / 32 * l.eyes));
		const unsigned eye = unsigned(rem / int(l.width / 32));
		const int tx = rem % int(l.width / 32);
		const auto & v = vectors[scalar_region_index(l, tx * 32 + 16, ty * 32 + 16)];
		for (int y = 0; y < 32; ++y)
			for (int x = 0; x < 32; ++x)
			{
				const uint8_t *src = nullptr;
				motion_pixel(l, old, info, eye, tx * 32 + x + v.dx, ty * 32 + y + v.dy, src);
				const size_t p = size_t(dst) + (size_t(y) * 32 + x) * 4;
				for (unsigned c = 0; c < 4; ++c) now[p + c] = src ? src[c] : 0;
			}
	}
	(void)x0;
	(void)y0;
	return now;
}

static motion_vector scalar_region(layout l, std::span<const uint8_t> old, const motion_native_info & oi,
		std::span<const uint8_t> now, const motion_native_info & ni, unsigned region)
{
	struct sample { uint8_t eye; int x, y; const uint8_t * current; };
	std::vector<sample> samples;
	const int x0 = int((l.width - l.native_side) / 2) & ~31, y0 = int((l.height - l.native_side) / 2) & ~31;
	const int split_x = int(l.width / 64 * 32), split_y = int(l.height / 64 * 32);
	const int rx = int(region & 1u), ry = int(region >> 1u);
	const int left = rx ? split_x : x0, right = rx ? x0 + int(l.native_side) : split_x;
	const int top = ry ? split_y : y0, bottom = ry ? y0 + int(l.native_side) : split_y;
	for (unsigned eye = 0; eye < l.eyes; ++eye)
		for (int y = top + 16; y < bottom - 16; y += 8)
			for (int x = left + 16; x < right - 16; x += 8)
			{
				const uint8_t *a, *b;
				if (motion_pixel(l, now, ni, eye, x, y, a) && motion_pixel(l, old, oi, eye, x, y, b))
					samples.push_back({uint8_t(eye), x, y, a});
			}
	motion_vector best;
	best.sad = std::numeric_limits<double>::max();
	auto test = [&](int dx, int dy) {
		uint64_t cost = 0;
		size_t hits = 0;
		for (const auto & s: samples)
		{
			const uint8_t *b;
			if (!motion_pixel(l, old, oi, s.eye, s.x + dx, s.y + dy, b)) continue;
			cost += unsigned(std::abs(int(s.current[0]) - int(b[0]))) +
			        unsigned(std::abs(int(s.current[1]) - int(b[1]))) +
			        unsigned(std::abs(int(s.current[2]) - int(b[2])));
			++hits;
		}
		if (samples.empty() || hits < (samples.size() + 1) / 2) return;
		const double sad = double(cost) / double(hits * 3);
		if (sad < best.sad || (sad == best.sad && std::abs(dx) + std::abs(dy) < std::abs(best.dx) + std::abs(best.dy)))
			best = {dx, dy, sad, hits};
	};
	for (int dy = -motion_search_radius; dy <= motion_search_radius; dy += 4)
		for (int dx = -motion_search_radius; dx <= motion_search_radius; dx += 4) test(dx, dy);
	const int cx = best.dx, cy = best.dy;
	for (int dy = std::max(-motion_search_radius, cy - 3); dy <= std::min(motion_search_radius, cy + 3); ++dy)
		for (int dx = std::max(-motion_search_radius, cx - 3); dx <= std::min(motion_search_radius, cx + 3); ++dx) test(dx, dy);
	return best;
}

static std::vector<uint8_t> scalar_regions_residual(layout l, std::span<const uint8_t> old,
		const motion_native_info & oi, std::span<const uint8_t> now, const motion_native_info & ni,
		const motion_region_vectors & vectors)
{
	std::vector<uint8_t> result(now.begin(), now.end());
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile)
	{
		const int32_t dst = ni.pixel[tile];
		if (dst < 0) continue;
		const int ty = int(tile / (l.width / 32 * l.eyes)), rem = int(tile % (l.width / 32 * l.eyes));
		const unsigned eye = unsigned(rem / int(l.width / 32));
		const int tx = rem % int(l.width / 32);
		const auto & v = vectors[scalar_region_index(l, tx * 32 + 16, ty * 32 + 16)];
		for (int y = 0; y < 32; ++y)
			for (int x = 0; x < 32; ++x)
			{
				const uint8_t *src = nullptr;
				motion_pixel(l, old, oi, eye, tx * 32 + x + v.dx, ty * 32 + y + v.dy, src);
				const size_t p = size_t(dst) + (size_t(y) * 32 + x) * 4;
				for (unsigned c = 0; c < 4; ++c) result[p + c] = uint8_t(now[p + c] - (src ? src[c] : 0));
			}
	}
	return result;
}

static void test_regions_wire()
{
	motion_region_vectors vectors{};
	vectors[0] = {-16, 16, 0, 0}; vectors[1] = {16, -16, 0, 0};
	vectors[2] = {7, -3, 0, 0}; vectors[3] = {-2, 9, 0, 0};
	const std::array<uint8_t, 3> body{1, 2, 3};
	auto wire = make_motion_regions_wire(vectors, 65535, body);
	auto parsed = parse_motion_regions_wire(wire);
	assert(wire.size() == motion_regions_header_bytes + body.size() && parsed && parsed->reference == 65535);
	assert(parsed->vectors[0].dx == -16 && parsed->vectors[0].dy == 16 && parsed->vectors[3].dx == -2 && parsed->vectors[3].dy == 9);
	assert(std::equal(parsed->body.begin(), parsed->body.end(), body.begin(), body.end()));
	assert(make_motion_regions_wire(motion_region_vectors{{motion_vector{17, 0}, {}, {}, {}}}, 1, body).empty());
	for (size_t n: {size_t(0), size_t(39), wire.size() - 1}) assert(!parse_motion_regions_wire(std::span<const uint8_t>(wire).first(n)));
	auto bad = wire; bad[4] = 1; assert(!parse_motion_regions_wire(bad));
	bad = wire; bad[12] = 2; assert(!parse_motion_regions_wire(bad));
	bad = wire; bad[16] = 3; assert(!parse_motion_regions_wire(bad));
	bad = wire; bad[10] = 1; assert(!parse_motion_regions_wire(bad));
	bad = wire; bad[24] = 17; assert(!parse_motion_regions_wire(bad));
	bad = wire; bad[24] = 0; bad[25] = 0; bad[26] = 0; bad[27] = 0x80; assert(!parse_motion_regions_wire(bad));
	bad = wire; bad[20]++; assert(!parse_motion_regions_wire(bad));
	bad = wire; bad.push_back(0); assert(!parse_motion_regions_wire(bad));
}

int regions_tests_main()
{
	test_regions_wire();
	auto l = regions_layout();
	assert(motion_regions_layout(l));
	auto old = make_frame(l);
	motion_native_info oi;
	assert(build_motion_native(l, old, oi));
	motion_region_vectors truth{};
	truth[0] = {8, 0, 0, 0}; truth[1] = {-8, 0, 0, 0};
	truth[2] = {0, 8, 0, 0}; truth[3] = {0, -8, 0, 0};
	auto now = make_regions_frame(l, old, oi, truth);
	motion_native_info ni;
	assert(build_motion_native(l, now, ni));
	const auto estimated = estimate_motion_regions(l, old, oi, now, ni);
	for (unsigned i = 0; i < 4; ++i)
	{
		const auto expected = scalar_region(l, old, oi, now, ni, i);
		assert(estimated[i].dx == expected.dx && estimated[i].dy == expected.dy &&
		       estimated[i].sad == expected.sad && estimated[i].hits == expected.hits);
		assert(estimated[i].dx == truth[i].dx && estimated[i].dy == truth[i].dy);
	}
	auto expected_residual = scalar_regions_residual(l, old, oi, now, ni, estimated);
	auto residual = motion_regions_residual(l, old, oi, now, ni, estimated);
	assert(residual == expected_residual);
	assert(restore_motion_regions(l, old, oi, residual, estimated) && residual == now);
	auto wire = make_motion_regions_wire(estimated, 7, residual);
	auto parsed = parse_motion_regions_wire(wire);
	assert(parsed && parsed->reference == 7 && parsed->body.size() == residual.size());

	// Matching vectors produce exactly the existing global residual bytes.
	auto same = make_frame(l, 4, -4);
	motion_native_info si;
	assert(build_motion_native(l, same, si));
	motion_region_vectors uniform{};
	for (auto & v: uniform) v = {4, -4, 0, 0};
	auto regional = motion_regions_residual(l, old, oi, same, si, uniform);
	auto global = motion_residual(l, old, oi, same, si, 4, -4);
	assert(regional == global);
	assert(restore_motion_regions(l, old, oi, regional, uniform) && regional == same);

	// Flat ties choose zero; sparse edge metadata still reconstructs exact bytes.
	auto flat_old = make_flat_frame(l), flat_now = make_flat_frame(l);
	motion_native_info fo, fn;
	assert(build_motion_native(l, flat_old, fo) && build_motion_native(l, flat_now, fn));
	const auto flat = estimate_motion_regions(l, flat_old, fo, flat_now, fn);
	for (const auto & v: flat) assert(v.dx == 0 && v.dy == 0 && v.sad == 0);
	auto sparse_old = old, sparse_now = now;
	sparsify(l, sparse_old, 0); sparsify(l, sparse_now, 2);
	assert(build_motion_native(l, sparse_old, fo) && build_motion_native(l, sparse_now, fn));
	const auto sparse_estimate = estimate_motion_regions(l, sparse_old, fo, sparse_now, fn);
	for (unsigned i = 0; i < 4; ++i)
	{
		const auto expected = scalar_region(l, sparse_old, fo, sparse_now, fn, i);
		assert(sparse_estimate[i].dx == expected.dx && sparse_estimate[i].dy == expected.dy &&
		       sparse_estimate[i].sad == expected.sad && sparse_estimate[i].hits == expected.hits);
	}
	auto sparse_residual = motion_regions_residual(l, sparse_old, fo, sparse_now, fn, estimated);
	assert(!sparse_residual.empty() && restore_motion_regions(l, sparse_old, fo, sparse_residual, estimated) && sparse_residual == sparse_now);

	// Unaligned image center uses the same tile-aligned native origin as the encoder.
	auto uneven = l;
	uneven.width = 544;
	uneven.height = 544;
	auto uneven_old = make_aligned_regions_base(uneven);
	motion_native_info uo;
	assert(build_motion_native(uneven, uneven_old, uo));
	auto uneven_now = make_regions_frame(uneven, uneven_old, uo, truth);
	motion_native_info un;
	assert(build_motion_native(uneven, uneven_now, un));
	const auto uneven_estimate = estimate_motion_regions(uneven, uneven_old, uo, uneven_now, un);
	for (unsigned i = 0; i < 4; ++i)
	{
		const auto expected = scalar_region(uneven, uneven_old, uo, uneven_now, un, i);
		assert(uneven_estimate[i].dx == expected.dx && uneven_estimate[i].dy == expected.dy &&
		       uneven_estimate[i].sad == expected.sad && uneven_estimate[i].hits == expected.hits);
		assert(uneven_estimate[i].dx == truth[i].dx && uneven_estimate[i].dy == truth[i].dy);
	}
	auto uneven_residual = motion_regions_residual(uneven, uneven_old, uo, uneven_now, un, uneven_estimate);
	assert(restore_motion_regions(uneven, uneven_old, uo, uneven_residual, uneven_estimate) && uneven_residual == uneven_now);

	// Reject undersized geometry and hostile signed vectors without abs(INT_MIN).
	auto too_small = l;
	too_small.width = too_small.height = 128;
	assert(!motion_regions_layout(too_small));
	assert(estimate_motion_regions(too_small, old, oi, now, ni)[0].sad == std::numeric_limits<double>::max());
	auto hostile = estimated;
	hostile[0].dx = std::numeric_limits<int>::min();
	assert(motion_regions_residual(l, old, oi, now, ni, hostile).empty());
	auto hostile_residual = expected_residual;
	assert(!restore_motion_regions(l, old, oi, hostile_residual, hostile));
	assert(make_motion_regions_wire(hostile, 7, std::span<const uint8_t>{}).empty());

	auto wrong_layout = l; wrong_layout.motion_regions = false;
	assert(!motion_regions_layout(wrong_layout));
	assert(motion_regions_residual(wrong_layout, old, oi, now, ni, estimated).empty());
	std::puts("direct motion regions: ok");
}
