// GPU-versus-CPU conformance for the two kernels where a silent mistake is
// plausible: the ballot-derived rANS read pointer, and the packed-int16 LDS
// transpose in the transform. PAPER 3.9 makes the CPU side normative.
#include "nxb_bench.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace nxb {

// Keep in step with the generator in Bench::init.
void genCoefficients(std::vector<int16_t>& c, int tileCount, uint64_t seed)
{
    c.resize(size_t(tileCount) * 4096);
    uint64_t s = seed;
    auto rnd = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 32); };
    for (size_t i = 0; i < c.size(); ++i)
    {
        int pos = int(i & 63);
        uint32_t r = rnd();
        int mag = (pos == 0) ? int(r % 40u) : ((r % 100u < 60u) ? 0 : int(r % 9u));
        c[i] = int16_t((r & 0x10000u) ? -mag : mag);
    }
}

namespace {

constexpr int C4 = 362, C2 = 473, S2 = 196;
constexpr int C1 = 502, S1 = 100, C3 = 426, S3 = 284;

inline int rsh(int x, int s) { return (x + (1 << (s - 1))) >> s; }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Byte-for-byte the same flow graph as nxb_idct8 in shaders/nxb_common.glsl.
void idct8(int F[8])
{
    int a = C4 * (F[0] + F[4]);
    int b = C4 * (F[0] - F[4]);
    int c = C2 * F[2] + S2 * F[6];
    int d = S2 * F[2] - C2 * F[6];

    int e0 = a + c, e1 = b + d, e2 = b - d, e3 = a - c;

    int t1 = C1 * F[1] + S1 * F[7];
    int t7 = C1 * F[7] - S1 * F[1];
    int t3 = C3 * F[3] + S3 * F[5];
    int t5 = C3 * F[5] - S3 * F[3];

    int u1 = t1 + t3, u3 = t1 - t3, u7 = t7 + t5, u5 = t7 - t5;

    int o0 = u1;
    int o1 = rsh(C4 * (u3 - u7), 9);
    int o2 = rsh(C4 * (u3 + u7), 9);
    int o3 = -u5;

    F[0] = e0 + o0; F[7] = e0 - o0;
    F[1] = e1 + o1; F[6] = e1 - o1;
    F[2] = e2 + o2; F[5] = e2 - o2;
    F[3] = e3 + o3; F[4] = e3 - o3;
}

// Reference Pass B without prediction, for one tile. Output is the 64x64
// residual mapped exactly as passb.comp with NXB_PREDICT 0 writes it.
void refTile(const int16_t* coef, int scale, uint8_t out[64][64])
{
    int stage1[64][64];   // [column][row], the transposed layout

    for (int b = 0; b < 64; ++b)
    {
        int bx = b & 7, by = b >> 3;
        for (int r = 0; r < 8; ++r)
        {
            int F[8];
            for (int cIdx = 0; cIdx < 8; ++cIdx)
                F[cIdx] = clampi(rsh(int(coef[b * 64 + r * 8 + cIdx]) * scale, 4), -8191, 8191);
            idct8(F);
            for (int col = 0; col < 8; ++col)
                stage1[bx * 8 + col][by * 8 + r] =
                    clampi(rsh(F[col], 8), -32767, 32767);
        }
    }

    for (int b = 0; b < 64; ++b)
    {
        int bx = b & 7, by = b >> 3;
        for (int col = 0; col < 8; ++col)
        {
            int F[8];
            for (int k = 0; k < 8; ++k) F[k] = stage1[bx * 8 + col][by * 8 + k];
            idct8(F);
            for (int k = 0; k < 8; ++k)
            {
                int res = clampi(rsh(F[k], 12), -2048, 2048);
                out[by * 8 + k][bx * 8 + col] = uint8_t(clampi(res + 128, 0, 255));
            }
        }
    }
}

int qstep16ref(int qp)
{
    static const int base[6] = {16, 18, 20, 23, 25, 29};
    int shift = qp / 6;
    if (shift > 20) shift = 20;
    return base[qp % 6] << shift;
}

} // namespace

bool Bench::verifyPassA(std::string* msg)
{
    char buf[256];
    if (!available(K4_RANS, msg)) return false;

    // Rebuild the streams with the expected symbols retained. Deterministic,
    // so it reproduces exactly what was uploaded.
    RansStream expect = ransBuildStreams(tables_, tileCount_, symsPerLane_, cfg_.seed, true);

    // Zero the coefficient buffer first so an unwritten slot cannot pass by
    // accident.
    {
        std::vector<int16_t> zero(size_t(tileCount_) * 4096, 0);
        ctx_->upload(coef_, zero.data(), VkDeviceSize(zero.size() * 2));
    }

    ctx_->oneShot([&](VkCommandBuffer cmd) {
        resetQueries(cmd, 0);
        recordKernel(cmd, K4_RANS, 0);
    });

    Buffer host = ctx_->createBuffer(coef_.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ctx_->oneShot([&](VkCommandBuffer cmd) {
        VkBufferCopy c{0, 0, coef_.size};
        vkCmdCopyBuffer(cmd, coef_.buf, host.buf, 1, &c);
    });

    const int16_t* got = reinterpret_cast<const int16_t*>(host.mapped);
    int nPerBlock = symsPerLane_ / kRansLanes;
    long bad = 0;
    int firstTile = -1, firstLane = -1, firstIdx = -1;

    for (int tile = 0; tile < tileCount_ && bad < 8; ++tile)
        for (int l = 0; l < kRansLanes && bad < 8; ++l)
        {
            const std::vector<int16_t>& want = expect.expect[size_t(tile)].value[l];
            for (int idx = 0; idx < symsPerLane_; ++idx)
            {
                int blk = idx / nPerBlock;
                int k = idx % nPerBlock;
                size_t at = size_t(tile) * 4096 + size_t(l) * 64 + size_t(blk) * 512 + size_t(k);
                if (got[at] != want[size_t(idx)])
                {
                    if (bad == 0) { firstTile = tile; firstLane = l; firstIdx = idx; }
                    ++bad;
                    break;
                }
            }
        }

    ctx_->destroyBuffer(host);

    if (bad)
    {
        snprintf(buf, sizeof buf,
                 "Pass A MISMATCH: first at tile %d lane %d symbol %d (>=%ld lanes wrong)",
                 firstTile, firstLane, firstIdx, bad);
        if (msg) *msg = buf;
        return false;
    }
    snprintf(buf, sizeof buf, "Pass A bit-exact: %d tiles x %d lanes x %d symbols",
             tileCount_, kRansLanes, symsPerLane_);
    if (msg) *msg = buf;
    return true;
}

bool Bench::verifyPassB(std::string* msg)
{
    char buf[256];

    // Restore the CPU coefficients (a K4 pass may have overwritten them).
    std::vector<int16_t> coefCpu;
    genCoefficients(coefCpu, tileCount_, cfg_.seed);
    ctx_->upload(coef_, coefCpu.data(), VkDeviceSize(coefCpu.size() * 2));

    ctx_->oneShot([&](VkCommandBuffer cmd) {
        resetQueries(cmd, 0);
        recordKernel(cmd, K3_IDCT, 0);
    });

    // Read back the first tile row only: 2048 x 64 pixels is enough to cover
    // every block position and every thread mapping, and keeps the copy small.
    const uint32_t rows = 64;
    VkDeviceSize bytes = VkDeviceSize(cfg_.width) * rows * 4;
    Buffer host = ctx_->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ctx_->oneShot([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = outImg_.img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {uint32_t(cfg_.width), rows, 1};
        vkCmdCopyImageToBuffer(cmd, outImg_.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               host.buf, 1, &c);

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    outImg_.layout = VK_IMAGE_LAYOUT_GENERAL;

    const uint8_t* got = reinterpret_cast<const uint8_t*>(host.mapped);
    int scale = (qstep16ref(cfg_.qp) * 16) >> 4;

    long bad = 0;
    int firstTile = -1, firstX = -1, firstY = -1, gotV = 0, wantV = 0;
    uint8_t ref[64][64];
    for (int tx = 0; tx < tilesX_ && bad == 0; ++tx)
    {
        refTile(&coefCpu[size_t(tx) * 4096], scale, ref);
        for (int y = 0; y < 64 && bad == 0; ++y)
            for (int x = 0; x < 64; ++x)
            {
                size_t at = (size_t(y) * size_t(cfg_.width) + size_t(tx * 64 + x)) * 4;
                if (got[at] != ref[y][x])
                {
                    ++bad;
                    firstTile = tx; firstX = x; firstY = y;
                    gotV = got[at]; wantV = ref[y][x];
                    break;
                }
            }
    }

    ctx_->destroyBuffer(host);

    if (bad)
    {
        snprintf(buf, sizeof buf,
                 "Pass B MISMATCH: tile %d pixel (%d,%d): got %d want %d",
                 firstTile, firstX, firstY, gotV, wantV);
        if (msg) *msg = buf;
        return false;
    }
    snprintf(buf, sizeof buf, "Pass B bit-exact: %d tiles x 64x64 pixels checked", tilesX_);
    if (msg) *msg = buf;
    return true;
}

} // namespace nxb
