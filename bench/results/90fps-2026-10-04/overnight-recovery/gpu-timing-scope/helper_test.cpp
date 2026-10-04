// g++ -std=c++23 -I server/encoder tests/astc_gpu_timing_test.cpp -o /tmp/astc_gpu_timing_test && /tmp/astc_gpu_timing_test
#include "astc_gpu_timing.h"

#include <cassert>
#include <limits>

int main()
{
	using wivrn::astc_gpu_timing::elapsed_ms;
	assert(elapsed_ms(17, 17, 64, 1'000'000.0) == 0.0);
	assert(elapsed_ms(10, 42, 32, 1'000'000.0) == 32.0);
	assert(elapsed_ms(1, 0, 1, 1'000'000.0) == 1.0);
	assert(elapsed_ms(0x1e, 0x2, 4, 1'000'000.0) == 4.0);
	assert(elapsed_ms(0xfffffff0, 0x10, 32, 1'000'000.0) == 32.0);
	assert(elapsed_ms(std::numeric_limits<uint64_t>::max() - 9, 5, 64, 1'000'000.0) == 15.0);
	assert(!elapsed_ms(0, 1, 0, 1.0));
	assert(!elapsed_ms(0, 1, 65, 1.0));
	assert(!elapsed_ms(0, 1, 32, 0.0));
	assert(!elapsed_ms(0, 1, 32, -1.0));
	assert(!elapsed_ms(0, 1, 32, std::numeric_limits<double>::infinity()));
	assert(!elapsed_ms(0, 1, 32, std::numeric_limits<double>::quiet_NaN()));
	assert(!elapsed_ms(0, std::numeric_limits<uint64_t>::max(), 64,
	                   std::numeric_limits<double>::max()));
}
