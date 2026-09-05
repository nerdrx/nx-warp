// Headless host CLI: the same kernels as the Android app, no swapchain, no
// window, against whatever Vulkan ICD the loader hands us (lavapipe in CI,
// RADV or amdgpu on a workstation).
//
// Its job is to let the kernels be iterated without a phone in the room. The
// numbers it produces are NOT the Phase 0 gate -- the gate is the Pico 4.
#include "nxb_bench.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

using namespace nxb;

namespace {

void usage()
{
    printf(
"nxbench-host -- NX Warp Phase 0 kernels, headless\n"
"\n"
"  --kernels LIST   comma-separated: k1,k2,k2b,k3,k4,k5,k6 or 'all' (default k1..k5)\n"
"  --frames N       measured frames per kernel (default 600)\n"
"  --warmup N       warm-up frames per kernel (default 120)\n"
"  --width N        frame width  (default 2048)\n"
"  --height N       frame height (default 4096, i.e. 2 views of 2048^2 stacked)\n"
"  --no-cotenant    do not run the dummy reprojection pass\n"
"  --thermal SEC    thermal mode: run K5 for SEC seconds and report per-minute p50\n"
"  --selftest       verify Pass A and Pass B against the CPU reference, then exit\n"
"  --k1-sweep [n]   time the K1 copy variants one dispatch at a time, then exit\n"
"  --out PATH       write the result JSON here (default ./nxwarp-phase0-host.json)\n"
"  --label TEXT     free-form label recorded in the JSON\n"
"  --validation     enable VK_LAYER_KHRONOS_validation\n"
"  --subgroup-size N  force the rANS kernel to subgroup width N (portability test)\n"
"  --info           print the device capability probe and exit\n"
"  -h, --help       this\n");
}

int kidFromName(const std::string& s)
{
    if (s == "k1")  return K1_COPY;
    if (s == "k2")  return K2_GATHER4;
    if (s == "k2b") return K2B_SAMPLER;
    if (s == "k3")  return K3_IDCT;
    if (s == "k4")  return K4_RANS;
    if (s == "k5")  return K5_FULL;
    if (s == "k6")  return K6_HYBRID;
    return -1;
}

} // namespace

int main(int argc, char** argv)
{
    Config cfg;
    cfg.kernelMask = (1u << K1_COPY) | (1u << K2_GATHER4) | (1u << K2B_SAMPLER) |
                     (1u << K3_IDCT) | (1u << K4_RANS) | (1u << K5_FULL);
    cfg.outPath = "nxwarp-phase0-host.json";
    bool selftest = false, infoOnly = false;
    int  k1sweep = 0;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--kernels")
        {
            std::string list = next();
            if (list == "all") cfg.kernelMask = (1u << KID_COUNT) - 1u;
            else
            {
                cfg.kernelMask = 0;
                size_t p = 0;
                while (p <= list.size())
                {
                    size_t q = list.find(',', p);
                    if (q == std::string::npos) q = list.size();
                    std::string tok = list.substr(p, q - p);
                    if (!tok.empty())
                    {
                        int k = kidFromName(tok);
                        if (k < 0) { fprintf(stderr, "unknown kernel '%s'\n", tok.c_str()); return 2; }
                        cfg.kernelMask |= 1u << k;
                    }
                    p = q + 1;
                }
            }
        }
        else if (a == "--frames")      cfg.frames = atoi(next().c_str());
        else if (a == "--warmup")      cfg.warmup = atoi(next().c_str());
        else if (a == "--width")       cfg.width  = atoi(next().c_str());
        else if (a == "--height")      cfg.height = atoi(next().c_str());
        else if (a == "--no-cotenant") cfg.cotenant = false;
        else if (a == "--thermal")     cfg.thermalSeconds = atof(next().c_str());
        else if (a == "--selftest")    selftest = true;
        else if (a == "--k1-sweep")
        {
            k1sweep = 9;
            if (i + 1 < argc && argv[i + 1][0] != '-') k1sweep = atoi(argv[++i]);
        }
        else if (a == "--out")         cfg.outPath = next();
        else if (a == "--label")       cfg.label = next();
        else if (a == "--validation")  cfg.validation = true;
        else if (a == "--subgroup-size") cfg.forceSubgroupSize = uint32_t(atoi(next().c_str()));
        else if (a == "--info")        infoOnly = true;
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }

    VkCtx ctx;
    if (!ctx.create({}, {}, cfg.validation)) return 1;

    NXB_LOG("device: %s", ctx.info.name.c_str());
    if (!ctx.info.driver.empty()) NXB_LOG("driver: %s", ctx.info.driver.c_str());
    NXB_LOG("subgroup size %u (min %u max %u), ballot %s, size control %s",
            ctx.info.subgroupSize, ctx.info.subgroupMin, ctx.info.subgroupMax,
            ctx.info.subgroupBallot ? "yes" : "NO",
            ctx.info.subgroupSizeControl ? "yes" : "no");
    NXB_LOG("max compute shared memory %u B, timestampPeriod %.3f ns, validBits %u",
            ctx.info.maxSharedMemory, double(ctx.info.timestampPeriod),
            ctx.info.timestampValidBits);

    if (infoOnly) { ctx.destroy(); return 0; }
    if (ctx.info.timestampValidBits == 0)
        NXB_LOG("WARNING: this queue family reports 0 timestamp valid bits; timings will be junk");

    Bench bench;
    if (!bench.init(ctx, cfg)) { ctx.destroy(); return 1; }

    if (k1sweep > 0)
    {
        bench.runK1Sweep(k1sweep);
        bench.destroy();
        ctx.destroy();
        return 0;
    }

    if (selftest)
    {
        std::string msgA, msgB;
        bool okB = bench.verifyPassB(&msgB);
        bool okA = bench.verifyPassA(&msgA);
        NXB_LOG("%s", msgB.c_str());
        NXB_LOG("%s", msgA.c_str());
        bench.destroy();
        ctx.destroy();
        return (okA && okB) ? 0 : 1;
    }

    Runner runner;
    runner.init(ctx, bench);

    RunHooks hooks;   // headless: nothing to acquire, nothing to present
    std::vector<KernelResult> results;

    if (cfg.thermalSeconds > 0.0)
    {
        results.push_back(runner.runPass(K5_FULL, cfg, hooks));
    }
    else
    {
        for (int k = 0; k < KID_COUNT; ++k)
        {
            if (!(cfg.kernelMask & (1u << k))) continue;
            if (k == K6_HYBRID)
            {
                KernelResult r;
                r.name = kidName(k);
                r.thresholdP50 = 2.0;
                r.skipReason = "K6 needs MediaCodec and AHardwareBuffer: Android only";
                results.push_back(r);
                continue;
            }
            results.push_back(runner.runPass(k, cfg, hooks));
        }
    }

    RunInfo info;
    info.device = ctx.info;
    info.cfg = cfg;
    info.platform = "host";
    info.mode = cfg.thermalSeconds > 0.0 ? "thermal" : "bench";
    info.verdict = verdictFor(results);

    std::string table = buildTable(info, results);
    fputs(table.c_str(), stdout);

    std::string json = buildJson(info, results);
    if (FILE* f = fopen(cfg.outPath.c_str(), "wb"))
    {
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
        NXB_LOG("wrote %s", cfg.outPath.c_str());
    }
    else
        fprintf(stderr, "could not write %s\n", cfg.outPath.c_str());

    runner.destroy();
    bench.destroy();
    ctx.destroy();
    return 0;
}
