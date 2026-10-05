// Additional root checks exercise ordering and invalid client timestamps through the actual controller.
#define main retained_bbr_test_main
#include "bitrate_bbr_test.cpp"
#undef main
int main()
{
    auto reversed_order = stereo_rate({{200000, 6000000, 6000000}, {200000, 0, 6000000}}).first;
    auto nested = stereo_rate({{200000, 0, 6000000}, {100000, 1500000, 3000000}}).first;
    auto negative = stereo_rate({{200000, 0, 6000000}, {200000, -12000000000LL, 6000000}}).first;
    CHECK(std::abs(reversed_order - 266666667.) < 1000);
    CHECK(std::abs(nested - 400000000.) < 1000);
    CHECK(std::abs(negative - 266666667.) < 1000);
    std::printf("reverse_order=%.3f nested=%.3f negative_excluded=%.3f Mbps; %d checks, %d failures\n", reversed_order/1e6, nested/1e6, negative/1e6, checks, failures);
    return failures ? 1 : 0;
}
