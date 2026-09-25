// Reuse the existing offscreen Vulkan fixture and enable its focused motion
// codec path. Compile with the fixture's normal -DNX_DIRECT_TEST_WIDTH=2176;
// native safety geometry is derived from 2160x2160 input.
#define NX_DIRECT_TEST_NATIVE
#define NX_DIRECT_TEST_MOTION
#define NX_DIRECT_PHOTO_FIXTURE
#include "nxwarp_direct_motion.h"
#include "nxwarp_direct_motion_regions.h"
#include "direct_blocks_gpu_test.cpp"
