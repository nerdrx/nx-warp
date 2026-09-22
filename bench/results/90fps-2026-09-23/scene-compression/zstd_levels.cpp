#include <zstd.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using bytes = std::vector<uint8_t>;
static bytes read_file(const std::string & path)
{
	std::ifstream f(path, std::ios::binary);
	return bytes((std::istreambuf_iterator<char>(f)), {});
}
static bytes pack(const bytes & raw, int level)
{
	bytes out(ZSTD_compressBound(raw.size()));
	const size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), level);
	if (ZSTD_isError(n)) std::abort();
	out.resize(n);
	return out;
}
static bytes unpack(const bytes & wire, size_t size)
{
	bytes out(size);
	const size_t n = ZSTD_decompress(out.data(), out.size(), wire.data(), wire.size());
	if (ZSTD_isError(n) || n != size) std::abort();
	return out;
}
template<class F> static std::pair<double, double> bench(F fn)
{
	std::vector<double> ms;
	for (int i = 0; i < 31; ++i) {
		auto start = std::chrono::steady_clock::now();
		fn();
		auto end = std::chrono::steady_clock::now();
		ms.push_back(std::chrono::duration<double, std::milli>(end - start).count());
	}
	std::sort(ms.begin(), ms.end());
	return {ms[15], ms[29]};
}
int main()
{
	std::ofstream csv("/tmp/nx-user-scenes/levels.csv");
	csv << "fixture,format,level,raw_bytes,compressed_bytes,saving_pct,encode_median_ms,encode_p95_ms,decode_median_ms,decode_p95_ms\n";
	for (const char * format : {"rgb565", "rgb888"}) for (const char * fixture : {"forest", "dark", "crowd"}) {
		const auto raw = read_file(std::string("/tmp/nx-user-scenes/reference500/") + fixture + "-" + format + ".nxdf");
		if (raw.empty()) continue;
		for (int level : {1, 3, 6, 9}) {
			const auto wire = pack(raw, level);
			if (unpack(wire, raw.size()) != raw) std::abort();
			const auto enc = bench([&] { auto ignored = pack(raw, level); (void)ignored; });
			const auto dec = bench([&] { auto ignored = unpack(wire, raw.size()); (void)ignored; });
			csv << fixture << ',' << format << ',' << level << ',' << raw.size() << ',' << wire.size() << ','
			     << std::fixed << std::setprecision(3) << 100.0 * (1.0 - double(wire.size()) / raw.size()) << ','
			     << enc.first << ',' << enc.second << ',' << dec.first << ',' << dec.second << '\n';
		}
	}
}
