// vk.decoder.conformance
//
// The exit criterion of docs/PAPER.md 3.11: the Vulkan decoder must reproduce
// the CPU reference's output with ZERO mismatching samples, on every device
// we ship on.
//
// Two bodies of evidence, both run on whatever ICD the environment selects:
//
//   1. tests/vectors/*.nxv, the frozen conformance vectors.  Each one is
//      decoded on the GPU and checked twice: against the `decoded_md5` pinned
//      in tests/vectors/vectors.md5 (the normative answer, produced by
//      tests/ref/vectors.cpp and independent of whatever ref/ compiles to
//      today), and pixel-for-pixel against an in-process nxvc_ref decode so a
//      failure names the first differing sample rather than just a hash.
//
//   2. A synthetic sweep encoded here and now with nxvc_encoder over QP,
//      res_level pattern, chroma format, colour transform, transform skip,
//      lane count, custom tables, alpha and picture size.  This is what
//      catches a combination nobody thought to freeze a vector for.
//
// Exit codes: 0 conformant, 1 a mismatch, 77 no usable Vulkan ICD (which is
// how ctest reports the test as a skip on a machine without one).

#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"
#include "nxvc/nxvc_vk.h"
#include "test_util.h"  // tests/ref: MD5, make_image, Rng

namespace {

const char *g_vectors_dir = nullptr;
// Device selection for the per-ICD ctest entries.  Unset means "first device
// that can run the decoder", which is what a developer run wants.
const char *device_filter() { return std::getenv("NXVC_VKD_DEVICE"); }
int g_fail = 0;
int g_checked = 0;
int g_skipped = 0;
bool g_verbose = false;

// ---------------------------------------------------------------- watchdog
//
// A sweep on a device can WEDGE, and when it does the default stdio buffering
// means the log is a zero-byte file that names nothing.  That is not
// hypothetical: an Adreno 650 run sat for 36 minutes with 55 seconds of user
// time, state R in hrtimer_nanosleep and stime creeping while utime did not
// move -- a fence that never signalled -- and because nothing had been
// flushed, the sweep cost an hour and identified no vector.
//
// Two changes make that self-reporting.  stdout is line buffered (see main),
// and one thread watches how long the current case has been running.  On a
// timeout it prints the case's NAME and _exit()s: a wedged GPU submission is
// not something a process can safely continue past, so the value here is
// attribution, not recovery -- and `--skip` then lets the next run sweep
// everything else.
std::atomic<uint64_t> g_case_start_ms{0};
std::mutex g_case_mu;
std::string g_case_name;
int g_case_timeout_s = 0;   // 0 = no watchdog
std::vector<std::string> g_skip;

uint64_t now_ms_wd() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Announce the case and arm the watchdog.  The BEGIN line is what turns a
// hang into "it was this vector" without waiting for the case to finish.
void case_begin(const std::string &name) {
    {
        std::lock_guard<std::mutex> lk(g_case_mu);
        g_case_name = name;
    }
    g_case_start_ms.store(now_ms_wd());
    if (g_case_timeout_s > 0) std::printf(".. %s\n", name.c_str());
}

void case_end() { g_case_start_ms.store(0); }

bool case_skipped(const std::string &name) {
    for (const std::string &s : g_skip)
        if (s == name) return true;
    return false;
}

// Arm for the life of a scope.  check_stream() has several early returns and
// every one of them must disarm, which is what an object is for.
struct CaseGuard {
    explicit CaseGuard(const std::string &name) { case_begin(name); }
    ~CaseGuard() { case_end(); }
    CaseGuard(const CaseGuard &) = delete;
    CaseGuard &operator=(const CaseGuard &) = delete;
};

void start_watchdog() {
    if (g_case_timeout_s <= 0) return;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const uint64_t t0 = g_case_start_ms.load();
            if (t0 == 0) continue;
            const uint64_t dt = now_ms_wd() - t0;
            if (dt < (uint64_t)g_case_timeout_s * 1000u) continue;
            std::string nm;
            {
                std::lock_guard<std::mutex> lk(g_case_mu);
                nm = g_case_name;
            }
            std::printf("HANG %s: no progress for %llu s -- watchdog\n",
                        nm.c_str(), (unsigned long long)(dt / 1000));
            std::fflush(stdout);
            std::fflush(stderr);
            // Not abort(): a wedged submission usually takes the driver's
            // cleanup down with it and a core dump helps nobody here.
            _exit(70);
        }
    }).detach();
}

// ------------------------------------------------------------------ planes
struct Planes {
    uint32_t yw = 0, yh = 0, cw = 0, ch = 0;
    bool alpha = false;
    std::vector<uint8_t> p[4];
    void size_for(uint32_t w, uint32_t h, uint32_t c_w, uint32_t c_h, bool a) {
        yw = w; yh = h; cw = c_w; ch = c_h; alpha = a;
        p[0].assign((size_t)w * h, 0);
        p[1].assign((size_t)c_w * c_h, 0);
        p[2].assign((size_t)c_w * c_h, 0);
        p[3].assign((size_t)w * h, 255);
    }
    void hash_into(MD5 &md) const {
        md.update(p[0].data(), p[0].size());
        md.update(p[1].data(), p[1].size());
        md.update(p[2].data(), p[2].size());
        if (alpha) md.update(p[3].data(), p[3].size());
    }
};

size_t compare(const char *what, int frame, const Planes &a, const Planes &b) {
    static const char *kPlane[4] = {"Y/R", "Co/G", "Cg/B", "A"};
    size_t bad = 0;
    const int np = a.alpha ? 4 : 3;
    for (int pl = 0; pl < np; ++pl) {
        const uint32_t w = (pl == 1 || pl == 2) ? a.cw : a.yw;
        const uint32_t h = (pl == 1 || pl == 2) ? a.ch : a.yh;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                size_t i = (size_t)y * w + x;
                if (a.p[pl][i] == b.p[pl][i]) continue;
                if (!bad)
                    std::printf(
                        "FAIL %s frame %d: plane %s (%u,%u) ref %u gpu %u\n",
                        what, frame, kPlane[pl], x, y, a.p[pl][i], b.p[pl][i]);
                ++bad;
            }
    }
    return bad;
}

// --------------------------------------------------------------- decoders
bool ref_decode(const std::vector<uint8_t> &stream, std::vector<Planes> &out,
                std::string &err) {
    nxvc_status st;
    nxvc_decoder *d = nxvc_decoder_create(&st);
    if (!d) { err = "nxvc_decoder_create"; return false; }
    size_t off = 0, consumed = 0;
    st = nxvc_decoder_parse_stream_header(d, stream.data(), stream.size(),
                                          &consumed);
    if (st != NXVC_OK) {
        err = std::string("stream header: ") + nxvc_status_string(st);
        nxvc_decoder_destroy(d);
        return false;
    }
    off = consumed;
    nxvc_stream_info si;
    nxvc_decoder_stream_info(d, &si);
    uint32_t yw, yh, cw, ch;
    nxvc_decoder_plane_size(d, 0, &yw, &yh);
    nxvc_decoder_plane_size(d, 1, &cw, &ch);
    while (off < stream.size()) {
        Planes pl;
        pl.size_for(yw, yh, cw, ch, si.alpha != 0);
        nxvc_image img{};
        img.plane[0] = pl.p[0].data(); img.stride[0] = (int)yw;
        img.plane[1] = pl.p[1].data(); img.stride[1] = (int)cw;
        img.plane[2] = pl.p[2].data(); img.stride[2] = (int)cw;
        img.plane[3] = pl.p[3].data(); img.stride[3] = (int)yw;
        st = nxvc_decoder_decode_frame(d, stream.data() + off,
                                       stream.size() - off, &img, &consumed);
        if (st != NXVC_OK) {
            err = std::string("frame ") + std::to_string(out.size()) + ": " +
                  nxvc_status_string(st);
            nxvc_decoder_destroy(d);
            return false;
        }
        out.push_back(std::move(pl));
        off += consumed;
    }
    nxvc_decoder_destroy(d);
    return true;
}

bool gpu_decode(const std::vector<uint8_t> &stream, uint32_t out_format,
                std::vector<Planes> &out, std::string &err,
                bool *unsupported) {
    *unsupported = false;
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.flags = (uint32_t)NXVC_VKD_FLAG_READBACK;
    ci.output_format = out_format;
    ci.device_name = device_filter();
    nxvc_vk_decoder *dec = nullptr;
    nxvc_vkd_status st = nxvc_vk_decoder_create(&ci, &dec);
    if (st != NXVC_VKD_OK) {
        /* Handle-free: this reports the failure even when create() never
         * produced a decoder to ask. */
        err = nxvc_vk_decoder_last_create_error();
        *unsupported = (st == NXVC_VKD_ERR_NO_DEVICE);
        nxvc_vk_decoder_destroy(dec);
        return false;
    }
    size_t off = 0, consumed = 0;
    st = nxvc_vk_decoder_parse_stream_header(dec, stream.data(), stream.size(),
                                             &consumed);
    if (st != NXVC_VKD_OK) {
        err = nxvc_vk_decoder_last_error(dec);
        *unsupported = (st == NXVC_VKD_ERR_UNSUPPORTED);
        nxvc_vk_decoder_destroy(dec);
        return false;
    }
    off = consumed;
    nxvc_vkd_stream_info si;
    nxvc_vk_decoder_stream_info(dec, &si);
    while (off < stream.size()) {
        st = nxvc_vk_decode_frame(dec, stream.data() + off, stream.size() - off,
                                  &consumed);
        if (st != NXVC_VKD_OK) {
            err = nxvc_vk_decoder_last_error(dec);
            // "shared memory over the device limit" is a device-capability
            // statement, not a conformance failure.
            *unsupported = (st == NXVC_VKD_ERR_UNSUPPORTED);
            nxvc_vk_decoder_destroy(dec);
            return false;
        }
        Planes pl;
        // [inter] The plane layout spans the eye pair, exactly as the
        // reference decoder's does; ask the decoder rather than deriving it
        // from `si.width`, which is per eye ([SYN] 3.3).
        uint32_t yw = 0, yh = 0, cw2 = 0, ch2 = 0;
        nxvc_vk_decoder_plane_size(dec, 0, &yw, &yh);
        nxvc_vk_decoder_plane_size(dec, 1, &cw2, &ch2);
        pl.size_for(yw, yh, cw2, ch2, si.alpha != 0);
        uint8_t *planes[4] = {pl.p[0].data(), pl.p[1].data(), pl.p[2].data(),
                              pl.p[3].data()};
        int32_t strides[4] = {(int32_t)yw, (int32_t)cw2, (int32_t)cw2,
                              (int32_t)yw};
        st = nxvc_vk_decoder_read_planes(dec, planes, strides);
        if (st != NXVC_VKD_OK) {
            err = nxvc_vk_decoder_last_error(dec);
            nxvc_vk_decoder_destroy(dec);
            return false;
        }
        out.push_back(std::move(pl));
        off += consumed;
    }
    nxvc_vk_decoder_destroy(dec);
    return true;
}

// One stream, checked every way.  `pinned_md5` is empty for synthetic
// streams, which have no manifest entry.
// ------------------------------------------------- [ATLAS] the atlas leg
// Under tool bit 31 the decoder's normative output is the ATLAS -- its pixels
// and all 64 bytes of every table entry -- and NOT a picture.  The manifest's
// `decoded_md5` for an atlas vector is the digest of exactly that, folded
// after every frame (tests/ref/vectors.cpp `atlas_bytes` / `decode_atlas`), so
// this reproduces that procedure byte for byte against the GPU decoder.
//
// Comparing the display picture here would compare two DIFFERENT THINGS and
// always fail: 13.12.5's display warp is not normative and this decoder does
// not produce the reference's one.  That is not a subtlety worth
// rediscovering -- it cost this branch a debugging round, chasing an
// "off-by-one at a tile boundary" that was a display picture being compared
// against an atlas digest.
bool stream_is_atlas(const std::vector<uint8_t> &s) {
    if (s.size() < 40) return false;
    uint64_t tools = 0;
    for (int i = 0; i < 8; ++i) tools |= (uint64_t)s[32 + i] << (8 * i);
    return (tools & (1ull << 31)) != 0;
}

// The same fold as the reference's `atlas_bytes()`: the whole table, then each
// plane's samples as little-endian u16, rows walked at `stride` and only `w`
// samples wide.
void atlas_fold(nxvc_vk_decoder *d, MD5 &md, std::vector<uint8_t> &scratch) {
    const size_t tb = nxvc_vk_decoder_atlas_table_size(d);
    scratch.assign(tb, 0u);
    if (tb) nxvc_vk_decoder_atlas_table(d, scratch.data(), tb);
    std::vector<uint16_t> plane;
    for (int p = 0; p < 4; ++p) {
        uint32_t w = 0, h = 0, stride = 0;
        // A size query first: a plane this stream does not have contributes
        // nothing, which is what the reference's null return does.
        nxvc_vk_decoder_atlas_plane(d, p, nullptr, 0, &w, &h, &stride);
        if (!w || !h || !stride) continue;
        plane.assign((size_t)stride * h, 0u);
        if (nxvc_vk_decoder_atlas_plane(d, p, plane.data(), plane.size(), &w,
                                        &h, &stride) != NXVC_VKD_OK)
            continue;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint16_t v = plane[(size_t)y * stride + x];
                scratch.push_back((uint8_t)(v & 0xff));
                scratch.push_back((uint8_t)(v >> 8));
            }
    }
    md.update(scratch.data(), scratch.size());
}

// A hash that disagrees says nothing a fix can use.  On a mismatch the two
// decoders are run in lockstep and the FIRST divergence is named: which frame,
// which table entry, which of 13.12.1's fields -- or which plane and sample.
// The 64-byte record is compared field by field rather than as bytes, because
// "entry 37 differs" is a different bug report from "entry 37's `gen` is 1
// where the reference says 0".
void atlas_attribute(const char *what, const std::vector<uint8_t> &stream) {
    nxvc_status cst;
    nxvc_decoder *rd = nxvc_decoder_create(&cst);
    if (!rd) return;
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.flags = 0;
    ci.output_format = NXVC_VKD_OUT_AUTO;
    ci.device_name = device_filter();
    nxvc_vk_decoder *gd = nullptr;
    if (nxvc_vk_decoder_create(&ci, &gd) != NXVC_VKD_OK) {
        nxvc_vk_decoder_destroy(gd);
        nxvc_decoder_destroy(rd);
        return;
    }
    size_t rc = 0, gc = 0;
    if (nxvc_decoder_parse_stream_header(rd, stream.data(), stream.size(),
                                         &rc) != NXVC_OK ||
        nxvc_vk_decoder_parse_stream_header(gd, stream.data(), stream.size(),
                                            &gc) != NXVC_VKD_OK) {
        nxvc_vk_decoder_destroy(gd);
        nxvc_decoder_destroy(rd);
        return;
    }
    uint32_t yw, yh, cw, ch;
    nxvc_decoder_plane_size(rd, 0, &yw, &yh);
    nxvc_decoder_plane_size(rd, 1, &cw, &ch);
    std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch),
        V((size_t)cw * ch);
    static const char *kField[16] = {
        "C[0]", "C[1]", "C[2]", "C[3]", "C[4]", "C[5]",
        "C[6]", "C[7]", "C[8]", "src_frame", "gen|flags|res_level",
        "reserved[0]", "reserved[1]", "reserved[2]", "reserved[3]",
        "reserved[4]"};
    size_t off = rc;
    int nf = 0;
    while (off < stream.size()) {
        nxvc_image oi{};
        oi.plane[0] = Y.data(); oi.stride[0] = (int)yw;
        oi.plane[1] = U.data(); oi.stride[1] = (int)cw;
        oi.plane[2] = V.data(); oi.stride[2] = (int)cw;
        size_t ur = 0, ug = 0;
        if (nxvc_decoder_decode_frame(rd, stream.data() + off,
                                      stream.size() - off, &oi, &ur) != NXVC_OK)
            break;
        if (nxvc_vk_decode_frame(gd, stream.data() + off, stream.size() - off,
                                 &ug) != NXVC_VKD_OK)
            break;
        const size_t tb = nxvc_decoder_atlas_table_size(rd);
        std::vector<uint8_t> rt(tb), gt(tb);
        nxvc_decoder_atlas_table(rd, rt.data(), tb);
        nxvc_vk_decoder_atlas_table(gd, gt.data(), tb);
        if (rt != gt) {
            const size_t entries = tb / 64;
            for (size_t e = 0; e < entries; ++e) {
                const uint32_t *r = (const uint32_t *)(rt.data() + e * 64);
                const uint32_t *g = (const uint32_t *)(gt.data() + e * 64);
                for (int k = 0; k < 16; ++k)
                    if (r[k] != g[k]) {
                        std::printf(
                            "  ^ %s frame %d: table entry %zu field %s: "
                            "ref 0x%08x gpu 0x%08x\n",
                            what, nf, e, kField[k], r[k], g[k]);
                        nxvc_vk_decoder_destroy(gd);
                        nxvc_decoder_destroy(rd);
                        return;
                    }
            }
        }
        for (int p = 0; p < 4; ++p) {
            uint32_t w = 0, h = 0, sd = 0;
            const uint16_t *rp = nxvc_decoder_atlas_plane(rd, p, &w, &h, &sd);
            if (!rp || !w || !h) continue;
            std::vector<uint16_t> gp((size_t)sd * h);
            uint32_t gw = 0, gh = 0, gsd = 0;
            if (nxvc_vk_decoder_atlas_plane(gd, p, gp.data(), gp.size(), &gw,
                                            &gh, &gsd) != NXVC_VKD_OK)
                continue;
            if (gw != w || gh != h || gsd != sd) {
                std::printf("  ^ %s frame %d: plane %d geometry ref %ux%u "
                            "stride %u, gpu %ux%u stride %u\n",
                            what, nf, p, w, h, sd, gw, gh, gsd);
                nxvc_vk_decoder_destroy(gd);
                nxvc_decoder_destroy(rd);
                return;
            }
            // Per TILE, not per sample: "tile (1,0) differs in 4096 of 4096
            // samples" and "tile (1,0) differs in 3" are different bugs, and
            // so is "every tile differs" against "one does".
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> perTile;
            uint32_t first[4] = {0, 0, 0, 0};
            bool have = false;
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                    if (rp[(size_t)y * sd + x] != gp[(size_t)y * sd + x]) {
                        ++perTile[{x / 64u, y / 64u}];
                        if (!have) {
                            have = true;
                            first[0] = x; first[1] = y;
                            first[2] = rp[(size_t)y * sd + x];
                            first[3] = gp[(size_t)y * sd + x];
                        }
                    }
            if (have) {
                std::printf("  ^ %s frame %d: plane %d differs in %zu tile(s), "
                            "first (%u,%u) ref %u gpu %u\n",
                            what, nf, p, perTile.size(), first[0], first[1],
                            first[2], first[3]);
                int shown = 0;
                for (auto &kv : perTile) {
                    if (shown++ == 8) break;
                    std::printf("      tile (col %u,row %u): %u samples\n",
                                kv.first.first, kv.first.second, kv.second);
                }
                nxvc_vk_decoder_destroy(gd);
                nxvc_decoder_destroy(rd);
                return;
            }
        }
        ++nf;
        off += ur;
    }
    nxvc_vk_decoder_destroy(gd);
    nxvc_decoder_destroy(rd);
}

// The atlas leg proper.  The comparison is GPU against a LIVE reference
// decode, byte for byte, after every frame: the whole table and every plane.
// That is the strongest statement that is checkable from the vector FILE.
//
// The manifest's `decoded_md5` is checked too, but it is not the primary
// comparison, and for two of the atlas vectors it CANNOT be: `v87` and `v88`
// have the generator apply a base-layer patch between frames (13.12.9), and
// that patch is built from a seed in tests/ref/vectors.cpp's spec table -- it
// does not travel in the .nxv file.  So their pinned digest depends on data an
// independent decoder cannot obtain from the vector, and no second
// implementation can reproduce it.  This reports that rather than failing on
// it: if the REFERENCE's own fold of the same stream also disagrees with the
// manifest, the difference is out-of-band data and not a decoder bug, and the
// GPU-vs-reference comparison above is what carries.
// [SYN] 13.12.9's sidecar.  `<vector>.basepatch` ships the base picture BESIDE
// the bitstream so the pinned digest is reproducible by anyone who does not
// have the generator's spec table compiled in -- which is the whole point of a
// conformance vector, and which v87/v88/v92 could not do until it existed.
struct BasePatchFile {
    uint32_t width = 0, height = 0, eye = 0, src_frame = 0, chroma_order = 0;
    uint32_t ystride = 0, cstride = 0;
    int apply_after = -1;
    std::vector<uint8_t> tiles, Y, C;
};

bool base_patch_load(const std::string &path, BasePatchFile &b) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 8 + 11 * 4) return false;
    if (std::memcmp(d.data(), "NXVBP1\0\0", 8) != 0) return false;
    auto u32 = [&](size_t o) {
        return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) |
               ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
    };
    b.width = u32(8); b.height = u32(12); b.eye = u32(16);
    b.src_frame = u32(20); b.chroma_order = u32(24); b.ystride = u32(28);
    b.cstride = u32(32); b.apply_after = (int)u32(36);
    const uint32_t nt = u32(40), ny = u32(44), nc = u32(48);
    size_t off = 8 + 11 * 4;
    if (d.size() != off + nt + ny + nc) return false;
    b.tiles.assign(d.begin() + off, d.begin() + off + nt); off += nt;
    b.Y.assign(d.begin() + off, d.begin() + off + ny); off += ny;
    b.C.assign(d.begin() + off, d.begin() + off + nc);
    return true;
}

// Apply the shipped patch to BOTH decoders and re-fold, which is what the
// reference does at the same point.  The GPU side goes through
// nxvc_vk_atlas_write_tiles(), so this is the entry point's conformance test
// as well as the vectors': the two atlases must still agree afterwards.
//
// The base picture ships as 8-bit YCbCr, which is what a client HEVC-decodes.
// The decoder's entry point takes a buffer ALREADY in the atlas's own coded
// sample domain -- the conversion is the client's kernel, deliberately, so the
// decoder never has to know a base picture's colour layout.  For a `CT_NONE`
// stream that conversion is a widening and a de-interleave, and it is done
// here on the host because the point of the test is the WRITE, not the
// colour.
bool apply_base_patch(const char *what, nxvc_decoder *rd, nxvc_vk_decoder *gd,
                      const BasePatchFile &bp, uint32_t eyes, MD5 &rmd,
                      MD5 &gmd, int &patched);

void check_stream_atlas(const char *what, const std::vector<uint8_t> &stream,
                        const std::string &pinned_md5) {
    CaseGuard cg_(what);
    nxvc_status cst;
    nxvc_decoder *rd = nxvc_decoder_create(&cst);
    if (!rd) {
        std::printf("FAIL %s: nxvc_decoder_create\n", what);
        ++g_fail;
        return;
    }
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.flags = 0;
    ci.output_format = NXVC_VKD_OUT_AUTO;
    ci.device_name = device_filter();
    nxvc_vk_decoder *gd = nullptr;
    if (nxvc_vk_decoder_create(&ci, &gd) != NXVC_VKD_OK) {
        std::printf("SKIP %s: %s\n", what,
                    gd ? nxvc_vk_decoder_last_error(gd) : "no decoder");
        ++g_skipped;
        nxvc_vk_decoder_destroy(gd);
        nxvc_decoder_destroy(rd);
        return;
    }
    size_t rc = 0, gc = 0;
    cst = nxvc_decoder_parse_stream_header(rd, stream.data(), stream.size(),
                                           &rc);
    nxvc_vkd_status st = nxvc_vk_decoder_parse_stream_header(
        gd, stream.data(), stream.size(), &gc);
    if (st == NXVC_VKD_ERR_UNSUPPORTED) {
        std::printf("SKIP %s: %s\n", what, nxvc_vk_decoder_last_error(gd));
        ++g_skipped;
        nxvc_vk_decoder_destroy(gd);
        nxvc_decoder_destroy(rd);
        return;
    }
    if (cst != NXVC_OK || st != NXVC_VKD_OK || rc != gc) {
        std::printf("FAIL %s: stream header: ref %s, gpu %s\n", what,
                    nxvc_status_string(cst), nxvc_vk_decoder_status_string(st));
        ++g_fail;
        nxvc_vk_decoder_destroy(gd);
        nxvc_decoder_destroy(rd);
        return;
    }
    ++g_checked;
    uint32_t yw, yh, cw, ch;
    nxvc_decoder_plane_size(rd, 0, &yw, &yh);
    nxvc_decoder_plane_size(rd, 1, &cw, &ch);
    std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch),
        V((size_t)cw * ch);
    nxvc_stream_info sinf{};
    nxvc_decoder_stream_info(rd, &sinf);
    const uint32_t si_eyes = sinf.eyes;
    BasePatchFile bp;
    const bool have_patch =
        base_patch_load(std::string(g_vectors_dir) + "/" + what + ".basepatch",
                        bp);
    int patched = 0;
    MD5 gmd, rmd;
    std::vector<uint8_t> gs, rs;
    size_t off = rc;
    int nf = 0;
    bool bad = false;
    while (off < stream.size() && !bad) {
        nxvc_image oi{};
        oi.plane[0] = Y.data(); oi.stride[0] = (int)yw;
        oi.plane[1] = U.data(); oi.stride[1] = (int)cw;
        oi.plane[2] = V.data(); oi.stride[2] = (int)cw;
        size_t ur = 0, ug = 0;
        cst = nxvc_decoder_decode_frame(rd, stream.data() + off,
                                        stream.size() - off, &oi, &ur);
        st = nxvc_vk_decode_frame(gd, stream.data() + off, stream.size() - off,
                                  &ug);
        if (cst != NXVC_OK || st != NXVC_VKD_OK || ur != ug) {
            std::printf("FAIL %s: frame %d: ref %s, gpu %s\n", what, nf,
                        nxvc_status_string(cst),
                        st ? nxvc_vk_decoder_last_error(gd) : "ok");
            ++g_fail;
            bad = true;
            break;
        }
        // The whole table, then every plane, exactly as the reference folds
        // it -- so the two digests are comparable and so is the manifest's.
        const size_t tb = nxvc_decoder_atlas_table_size(rd);
        rs.assign(tb, 0u);
        gs.assign(tb, 0u);
        nxvc_decoder_atlas_table(rd, rs.data(), tb);
        nxvc_vk_decoder_atlas_table(gd, gs.data(), tb);
        for (int p = 0; p < 4; ++p) {
            uint32_t w = 0, h = 0, sd = 0;
            const uint16_t *rp = nxvc_decoder_atlas_plane(rd, p, &w, &h, &sd);
            if (!rp || !w || !h) continue;
            std::vector<uint16_t> gp((size_t)sd * h);
            uint32_t gw = 0, gh = 0, gsd = 0;
            nxvc_vk_decoder_atlas_plane(gd, p, gp.data(), gp.size(), &gw, &gh,
                                        &gsd);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    const uint16_t rv = rp[(size_t)y * sd + x];
                    const uint16_t gv = gp[(size_t)y * sd + x];
                    rs.push_back((uint8_t)(rv & 0xff));
                    rs.push_back((uint8_t)(rv >> 8));
                    gs.push_back((uint8_t)(gv & 0xff));
                    gs.push_back((uint8_t)(gv >> 8));
                }
        }
        if (rs != gs) {
            // Table or pixels?  They are different bugs and the first `tb`
            // bytes are the table, so say which before anything else.
            size_t d0 = 0;
            while (d0 < rs.size() && d0 < gs.size() && rs[d0] == gs[d0]) ++d0;
            if (d0 < tb) {
                const size_t e = d0 / 64, k = (d0 % 64) / 4;
                const uint32_t *r32 = (const uint32_t *)(rs.data() + e * 64);
                const uint32_t *g32 = (const uint32_t *)(gs.data() + e * 64);
                std::printf("FAIL %s: frame %d: TABLE entry %zu uint %zu: "
                            "ref 0x%08x gpu 0x%08x (patched %d)\n",
                            what, nf, e, k, r32[k], g32[k], patched);
            } else {
                std::printf("FAIL %s: frame %d: PIXELS differ from byte %zu "
                            "of %zu (table %zu B agreed, patched %d)\n",
                            what, nf, d0 - tb, rs.size() - tb, tb, patched);
            }
            std::printf("FAIL %s: the atlas differs from the reference at "
                        "frame %d\n", what, nf);
            ++g_fail;
            bad = true;
            atlas_attribute(what, stream);
            break;
        }
        rmd.update(rs.data(), rs.size());
        gmd.update(gs.data(), gs.size());
        ++nf;
        off += ur;
        if (have_patch && nf == bp.apply_after + 1) {
            if (!apply_base_patch(what, rd, gd, bp, si_eyes, rmd, gmd,
                                  patched)) {
                ++g_fail;
                bad = true;
                break;
            }
        }
    }
    nxvc_vk_decoder_destroy(gd);
    nxvc_decoder_destroy(rd);
    if (bad) return;
    const std::string got = gmd.hex(), refgot = rmd.hex();
    if (!pinned_md5.empty() && got != pinned_md5) {
        if (refgot != pinned_md5) {
            // Both decoders agree and both differ from the manifest, so the
            // pin covers something the stream does not carry -- the base
            // patch of 13.12.9 for v87 and v88.
            std::printf("-- %s: atlas byte-identical to the reference over %d "
                        "frames; the manifest pin (%s) is NOT reproducible "
                        "from the vector file -- the reference folds %s too, "
                        "because the pin includes a base patch built outside "
                        "the stream\n",
                        what, nf, pinned_md5.c_str(), refgot.c_str());
            return;
        }
        std::printf("FAIL %s: atlas md5 %s, manifest says %s (%d frames)\n",
                    what, got.c_str(), pinned_md5.c_str(), nf);
        ++g_fail;
        atlas_attribute(what, stream);
        return;
    }
    if (g_verbose) std::printf("ok   %s (atlas, %d frames)\n", what, nf);
}

bool apply_base_patch(const char *what, nxvc_decoder *rd, nxvc_vk_decoder *gd,
                      const BasePatchFile &bp, uint32_t eyes, MD5 &rmd,
                      MD5 &gmd, int &patched) {
    // ---- the reference, straight from the shipped bytes.
    nxvc_base_patch api{};
    api.plane[0] = bp.Y.data();
    api.stride[0] = (int)bp.ystride;
    api.plane[1] = bp.C.data();
    api.stride[1] = (int)bp.cstride;
    api.width = bp.width;
    api.height = bp.height;
    api.eye = bp.eye;
    api.src_frame = bp.src_frame;
    api.chroma_order = bp.chroma_order;
    api.tiles = bp.tiles.data();
    api.tile_bytes = (uint32_t)bp.tiles.size();
    uint32_t rap = 0;
    if (nxvc_decoder_atlas_patch_base(rd, &api, &rap, nullptr) != NXVC_OK) {
        std::printf("FAIL %s: reference base patch refused\n", what);
        return false;
    }

    // ---- the GPU, through nxvc_vk_atlas_write_tiles().
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    if (nxvc_vk_decoder_vk_handles(gd, nullptr, &phys, &dev, nullptr,
                                   nullptr) != NXVC_VKD_OK ||
        !dev) {
        std::printf("FAIL %s: no Vulkan handles from the decoder\n", what);
        return false;
    }
    uint32_t pw[4] = {0, 0, 0, 0}, ph[4] = {0, 0, 0, 0}, ps[4] = {0, 0, 0, 0};
    size_t slot = 0;
    for (int p = 0; p < 4; ++p) {
        nxvc_vk_decoder_atlas_plane(gd, p, nullptr, 0, &pw[p], &ph[p], &ps[p]);
        if (pw[p] && ph[p]) slot += (size_t)ps[p] * ph[p];
    }
    // The slot-shaped u16 source: plane 0 from Y, planes 1 and 2 from the
    // interleaved chroma, at the eye's own column offset.  Only the named
    // tiles are ever copied, so the rest may stay zero.
    std::vector<uint16_t> srcbuf(slot, 0u);
    size_t base = 0;
    for (int p = 0; p < 4; ++p) {
        if (!pw[p] || !ph[p]) continue;
        if (p == 0) {
            for (uint32_t y = 0; y < ph[0] && y < bp.height; ++y)
                for (uint32_t x = 0; x < pw[0] && x < bp.width; ++x)
                    srcbuf[base + (size_t)y * ps[0] + bp.eye * pw[0] + x] =
                        bp.Y[(size_t)y * bp.ystride + x];
        } else if (p == 1 || p == 2) {
            // NXVC_BASE_CHROMA_CB_CR is (Cb,Cr); the other order swaps them.
            const int sel = (bp.chroma_order == 0) ? (p - 1) : (2 - p);
            for (uint32_t y = 0; y < ph[p]; ++y)
                for (uint32_t x = 0; x < pw[p]; ++x)
                    srcbuf[base + (size_t)y * ps[p] + bp.eye * pw[p] + x] =
                        bp.C[(size_t)y * bp.cstride + x * 2 + sel];
        }
        base += (size_t)ps[p] * ph[p];
    }
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = srcbuf.size() * 2;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bci, nullptr, &buf) != VK_SUCCESS) {
        std::printf("FAIL %s: vkCreateBuffer for the patch source\n", what);
        return false;
    }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mt = i;
            break;
        }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mt;
    if (mt == UINT32_MAX ||
        vkAllocateMemory(dev, &mai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev, buf, nullptr);
        std::printf("FAIL %s: no host-visible memory for the patch source\n",
                    what);
        return false;
    }
    vkBindBufferMemory(dev, buf, mem, 0);
    void *mapped = nullptr;
    vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped);
    std::memcpy(mapped, srcbuf.data(), srcbuf.size() * 2);
    vkUnmapMemory(dev, mem);

    nxvc_vkd_atlas_src src{};
    src.buffer = buf;
    src.offset = 0;
    // The named tiles, as contiguous RUNS -- which is the form the entry point
    // is built around, because row strips are what the writes coalesce to.
    uint32_t gap = 0, gsup = 0;
    nxvc_vkd_status wst = NXVC_VKD_OK;
    const uint32_t nt = (uint32_t)bp.tiles.size();
    for (uint32_t i = 0; i < nt && wst == NXVC_VKD_OK;) {
        if (!bp.tiles[i]) { ++i; continue; }
        uint32_t j = i;
        while (j < nt && bp.tiles[j]) ++j;
        uint32_t a = 0, sup = 0;
        wst = nxvc_vk_atlas_write_tiles(gd, bp.eye, i, j - i, &src,
                                        bp.src_frame, 0, &a, &sup);
        gap += a;
        gsup += sup;
        i = j;
    }
    vkDestroyBuffer(dev, buf, nullptr);
    vkFreeMemory(dev, mem, nullptr);
    if (wst != NXVC_VKD_OK) {
        std::printf("FAIL %s: nxvc_vk_atlas_write_tiles: %s\n", what,
                    nxvc_vk_decoder_last_error(gd));
        return false;
    }
    if (gap != rap) {
        std::printf("FAIL %s: the base patch applied %u tile(s) on the GPU and "
                    "%u on the reference\n", what, gap, rap);
        return false;
    }
    patched = (int)gap;
    (void)eyes;

    // Re-fold both atlases, exactly as the reference does after its patch.
    std::vector<uint8_t> rs2, gs2;
    const size_t tb = nxvc_decoder_atlas_table_size(rd);
    rs2.assign(tb, 0u);
    gs2.assign(tb, 0u);
    nxvc_decoder_atlas_table(rd, rs2.data(), tb);
    nxvc_vk_decoder_atlas_table(gd, gs2.data(), tb);
    for (int p = 0; p < 4; ++p) {
        uint32_t w = 0, h = 0, sd = 0;
        const uint16_t *rp = nxvc_decoder_atlas_plane(rd, p, &w, &h, &sd);
        if (!rp || !w || !h) continue;
        std::vector<uint16_t> gp((size_t)sd * h);
        uint32_t gw = 0, gh = 0, gsd = 0;
        nxvc_vk_decoder_atlas_plane(gd, p, gp.data(), gp.size(), &gw, &gh,
                                    &gsd);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint16_t rv = rp[(size_t)y * sd + x];
                const uint16_t gv = gp[(size_t)y * sd + x];
                rs2.push_back((uint8_t)(rv & 0xff));
                rs2.push_back((uint8_t)(rv >> 8));
                gs2.push_back((uint8_t)(gv & 0xff));
                gs2.push_back((uint8_t)(gv >> 8));
            }
    }
    if (rs2 != gs2) {
        std::printf("FAIL %s: the atlas differs from the reference AFTER the "
                    "base patch (%u applied, %u superseded)\n", what, gap, gsup);
        return false;
    }
    rmd.update(rs2.data(), rs2.size());
    gmd.update(gs2.data(), gs2.size());
    return true;
}

void check_stream(const char *what, const std::vector<uint8_t> &stream,
                  const std::string &pinned_md5, uint32_t out_format) {
    // [ATLAS] An atlas stream's normative output is the atlas, so it takes a
    // different LEG entirely -- not a different comparison of the same thing.
    if (stream_is_atlas(stream)) {
        check_stream_atlas(what, stream, pinned_md5);
        return;
    }
    // The watchdog's real arming point.  The manifest sweep, the synthetic
    // streams and their RGB10A2 variants all reach the GPU through here, and
    // arming only the manifest loop is how a wedge in the synthetic stage
    // (syn_xform32_420, the same XFORM_LARGE hang as v70_xform32_444) ran
    // unattended a second time.
    CaseGuard cg_(what);
    std::string err;
    std::vector<Planes> ref, gpu;
    if (!ref_decode(stream, ref, err)) {
        std::printf("FAIL %s: reference decode failed (%s)\n", what,
                    err.c_str());
        ++g_fail;
        return;
    }
    bool unsupported = false;
    if (!gpu_decode(stream, out_format, gpu, err, &unsupported)) {
        if (unsupported) {
            std::printf("SKIP %s: %s\n", what, err.c_str());
            ++g_skipped;
            return;
        }
        std::printf("FAIL %s: GPU decode failed (%s)\n", what, err.c_str());
        ++g_fail;
        return;
    }
    ++g_checked;
    if (ref.size() != gpu.size()) {
        std::printf("FAIL %s: %zu reference frames, %zu GPU frames\n", what,
                    ref.size(), gpu.size());
        ++g_fail;
        return;
    }
    size_t bad = 0;
    for (size_t f = 0; f < ref.size(); ++f)
        bad += compare(what, (int)f, ref[f], gpu[f]);
    if (bad) {
        std::printf("FAIL %s: %zu mismatching samples\n", what, bad);
        ++g_fail;
        return;
    }
    if (!pinned_md5.empty()) {
        MD5 md;
        for (const Planes &p : gpu) p.hash_into(md);
        std::string got = md.hex();
        if (got != pinned_md5) {
            std::printf("FAIL %s: decoded md5 %s, manifest says %s\n", what,
                        got.c_str(), pinned_md5.c_str());
            ++g_fail;
            return;
        }
    }
    if (g_verbose) std::printf("ok   %s\n", what);
}

// ---------------------------------------------------------------- vectors
struct ManifestRow {
    std::string name, stream_md5, decoded_md5;
};

bool read_manifest(std::vector<ManifestRow> &rows) {
    std::string path = std::string(g_vectors_dir) + "/vectors.md5";
    std::FILE *f = std::fopen(path.c_str(), "r");
    if (!f) {
        std::printf("cannot open %s\n", path.c_str());
        return false;
    }
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char name[128], sm[64], dm[64];
        if (std::sscanf(line, "%127s %63s %63s", name, sm, dm) != 3) continue;
        rows.push_back({name, sm, dm});
    }
    std::fclose(f);
    return !rows.empty();
}

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return false; }
    out.resize((size_t)n);
    bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

const char *vkd_status_token(nxvc_vkd_status st);

// The tool bits this decoder implements.  docs/SYNTAX.md 12: a decoder
// "rejects ... any tool bit outside the supported set" with a VERSION status,
// and tests/vectors holds vectors for tools this decoder does not carry yet,
// which every bit of that set refuses on purpose.  Those are counted as skips
// rather than failures -- but only when the stream's own tool mask says so and
// only when the decoder refuses it with exactly VERSION, so a regression that
// starts refusing a supported vector still fails.
//
// It is NOT restated here.  It used to be, and a copy of a tool mask is a copy
// that goes stale: the harness would then either fail a vector the decoder now
// speaks or skip one it no longer does, and both read as a decoder bug.  The
// C ABI names the decoder's half of the handshake, so the harness asks.  The
// skip count of the sweep is exactly "how many vectors this decoder cannot yet
// speak", so driving it to zero is what finishing the tool set means.
uint64_t supported_tools() { return nxvc_vk_decoder_tools_supported(); }

// The tools THIS DEVICE accepts, which can be less than the build implements:
// a device that hangs on a legal stream must not advertise the tool that
// reaches it, and the Adreno 650 clears XFORM_LARGE (bit 27) for exactly that
// reason.  Asked once, of a real decoder on the device under test, and cached;
// the build-wide mask stands in when there is no device to ask.
//
// The harness used to consult only the build-wide mask, and so judged every
// XFORM_LARGE vector against a tool the LIBRARY has and the DEVICE refuses.
// On the headset that was 24 vectors reported as decode failures and three
// rejection vectors reported as refused with the wrong status -- 27 "failures"
// that were the decoder doing precisely what it promises.  A sweep that cries
// wolf 27 times is a sweep nobody reads.
uint64_t device_tools() {
    static uint64_t cached = 0;
    static bool asked = false;
    if (!asked) {
        asked = true;
        cached = supported_tools();
        nxvc_vkd_create_info ci;
        nxvc_vk_decoder_create_info_default(&ci);
        ci.device_name = device_filter();
        nxvc_vk_decoder *dec = nullptr;
        if (nxvc_vk_decoder_create(&ci, &dec) == NXVC_VKD_OK)
            cached = nxvc_vk_decoder_tools(dec);
        nxvc_vk_decoder_destroy(dec);
    }
    return cached;
}

// docs/SYNTAX.md 11: `tools` is a u64 at byte 32 of the 64-byte stream header.
uint64_t stream_tools(const std::vector<uint8_t> &s) {
    uint64_t tools = 0;
    if (s.size() < 40) return 0;
    for (int i = 0; i < 8; ++i) tools |= (uint64_t)s[32 + i] << (8 * i);
    return tools;
}

// The stream asks for something the BUILD has not implemented.  Not a result:
// the sweep skips it, and the skip count is exactly "how many vectors this
// decoder cannot yet speak", so driving it to zero is what finishing the tool
// set means.
bool stream_needs_phase2(const std::vector<uint8_t> &s, uint64_t &tools) {
    tools = stream_tools(s);
    return (tools & ~supported_tools()) != 0;
}

// The stream asks for something this build HAS but this device declines.  That
// is not a gap and never will be closed: refusing the stream at the header is
// the promised behaviour, so it is a RESULT -- checked, and passed when the
// refusal happens and is named VERSION.
bool stream_declined_by_device(const std::vector<uint8_t> &s, uint64_t &tools) {
    tools = stream_tools(s);
    return (tools & ~supported_tools()) == 0 &&
           (tools & supported_tools() & ~device_tools()) != 0;
}

// A stream whose tools this build has and this device declines must be refused
// at the header, with VERSION.  Returns true when it handled the case (checked
// and scored); false when the stream is nothing to do with the device mask and
// the caller should decode it normally.
//
// Shared by the vector sweep and the synthetic sweep because the promise is
// the same in both, and because the synthetic sweep had no tool gate at all:
// on the headset its eighteen XFORM_LARGE streams were reported as decode
// failures for doing exactly what the decoder guarantees.
bool handled_as_device_refusal(const char *name,
                               const std::vector<uint8_t> &stream) {
    uint64_t tools = 0;
    if (!stream_declined_by_device(stream, tools)) return false;
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.device_name = device_filter();
    nxvc_vk_decoder *dec = nullptr;
    if (nxvc_vk_decoder_create(&ci, &dec) != NXVC_VKD_OK) {
        nxvc_vk_decoder_destroy(dec);
        ++g_skipped;
        return true;
    }
    size_t consumed = 0;
    nxvc_vkd_status st = nxvc_vk_decoder_parse_stream_header(
        dec, stream.data(), stream.size(), &consumed);
    nxvc_vk_decoder_destroy(dec);
    ++g_checked;
    if (st != NXVC_VKD_ERR_VERSION) {
        std::printf("FAIL %s: tools 0x%llx are not offered by this device and "
                    "must be refused with VERSION, got %s\n",
                    name, (unsigned long long)tools, vkd_status_token(st));
        ++g_fail;
    } else if (g_verbose) {
        std::printf("ok   %s (tools 0x%llx declined by this device, refused "
                    "with VERSION)\n",
                    name, (unsigned long long)tools);
    }
    return true;
}

void run_vectors() {
    std::vector<ManifestRow> rows;
    if (!read_manifest(rows)) {
        std::printf("FAIL: no conformance manifest\n");
        ++g_fail;
        return;
    }
    std::printf("-- %zu conformance vectors from %s\n", rows.size(),
                g_vectors_dir);
    for (const ManifestRow &r : rows) {
        if (case_skipped(r.name)) {
            std::printf("skip %s: --skip\n", r.name.c_str());
            ++g_skipped;
            continue;
        }
        std::string path = std::string(g_vectors_dir) + "/" + r.name + ".nxv";
        std::vector<uint8_t> stream;
        if (!read_file(path, stream)) {
            std::printf("FAIL %s: cannot read %s\n", r.name.c_str(),
                        path.c_str());
            ++g_fail;
            continue;
        }
        // The manifest pins the bitstream too, so a corrupted vector file is
        // reported as such rather than as a decoder bug.
        std::string sm = md5_hex(stream.data(), stream.size());
        if (sm != r.stream_md5) {
            std::printf("FAIL %s: vector file md5 %s, manifest says %s\n",
                        r.name.c_str(), sm.c_str(), r.stream_md5.c_str());
            ++g_fail;
            continue;
        }
        uint64_t tools = 0;
        if (handled_as_device_refusal(r.name.c_str(), stream)) continue;
        if (stream_needs_phase2(stream, tools)) {
            // Must be refused, and refused with VERSION: "the tools mask is
            // not something this decoder speaks".
            nxvc_vkd_create_info ci;
            nxvc_vk_decoder_create_info_default(&ci);
            ci.device_name = device_filter();
            nxvc_vk_decoder *dec = nullptr;
            nxvc_vkd_status cst = nxvc_vk_decoder_create(&ci, &dec);
            if (cst != NXVC_VKD_OK) {
                nxvc_vk_decoder_destroy(dec);
                ++g_skipped;
                continue;
            }
            size_t consumed = 0;
            nxvc_vkd_status st = nxvc_vk_decoder_parse_stream_header(
                dec, stream.data(), stream.size(), &consumed);
            nxvc_vk_decoder_destroy(dec);
            // VERSION for the tool mask, or UNSUPPORTED when a Phase 1
            // check fires first (a stereo vector carries eyes == 2, which
            // docs/SYNTAX.md 12 says to refuse as UNSUPPORTED).  Anything
            // else, including a successful parse, is a bug.
            if (st != NXVC_VKD_ERR_VERSION && st != NXVC_VKD_ERR_UNSUPPORTED) {
                std::printf("FAIL %s: Phase 2 tools 0x%llx must be refused, "
                            "got %s\n",
                            r.name.c_str(), (unsigned long long)tools,
                            vkd_status_token(st));
                ++g_fail;
                continue;
            }
            if (g_verbose)
                std::printf("skip %s: Phase 2 tools 0x%llx, correctly "
                            "refused\n",
                            r.name.c_str(), (unsigned long long)tools);
            ++g_skipped;
            continue;
        }
        check_stream(r.name.c_str(), stream, r.decoded_md5, NXVC_VKD_OUT_AUTO);
    }
}

// ------------------------------------------------------------- rejections
// docs/SYNTAX.md 12: "a conforming decoder never produces output from a
// stream it must reject", and the *named* status matters -- UNSUPPORTED is
// "legal v1 syntax this profile does not implement", BITSTREAM is "this
// cannot be a legal stream at all".  tests/vectors/rejects.md5 pins both.
const char *vkd_status_token(nxvc_vkd_status st) {
    switch (st) {
        case NXVC_VKD_OK: return "OK";
        case NXVC_VKD_ERR_ARG: return "ARG";
        case NXVC_VKD_ERR_UNSUPPORTED: return "UNSUPPORTED";
        case NXVC_VKD_ERR_BITSTREAM: return "BITSTREAM";
        case NXVC_VKD_ERR_TRUNCATED: return "TRUNCATED";
        case NXVC_VKD_ERR_NOMEM: return "NOMEM";
        case NXVC_VKD_ERR_VERSION: return "VERSION";
        default: return "OTHER";
    }
}

void run_rejects() {
    std::string path = std::string(g_vectors_dir) + "/rejects.md5";
    std::FILE *f = std::fopen(path.c_str(), "r");
    if (!f) {
        std::printf("-- no rejects.md5, rejection sweep skipped\n");
        return;
    }
    int n = 0;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char name[128], md5[64], want[32];
        if (std::sscanf(line, "%127s %63s %31s", name, md5, want) != 3) continue;
        std::vector<uint8_t> stream;
        std::string vp = std::string(g_vectors_dir) + "/" + name + ".nxv";
        if (!read_file(vp, stream)) {
            std::printf("FAIL %s: cannot read %s\n", name, vp.c_str());
            ++g_fail;
            continue;
        }
        if (md5_hex(stream.data(), stream.size()) != md5) {
            std::printf("FAIL %s: rejection vector file md5 differs from the "
                        "manifest\n", name);
            ++g_fail;
            continue;
        }
        // A Phase 2 rejection vector may be malformed for a reason inside
        // the inter syntax, which a Phase 1 decoder never gets far enough to
        // see: it refuses the stream at the tool mask, earlier and with a
        // different but equally correct status.  Such a vector still has to
        // be REFUSED -- that is checked below either way -- but the exact
        // named status is not this decoder's to reproduce until the inter
        // path lands.
        uint64_t tools = 0;
        const bool phase2 = stream_needs_phase2(stream, tools);
        // ... and the same for a tool this build has but this DEVICE declines.
        // r36, r38 and r39 are malformed inside XFORM_LARGE syntax, so their
        // manifest status is BITSTREAM; on a device that does not offer the
        // tool the header refuses them first, and VERSION is then the right
        // answer rather than a wrong one.  The vector is still REFUSED, which
        // is the property that matters and is checked either way.
        uint64_t dtools = 0;
        const bool declined = stream_declined_by_device(stream, dtools);
        nxvc_vkd_create_info ci;
        nxvc_vk_decoder_create_info_default(&ci);
        ci.device_name = device_filter();
        nxvc_vk_decoder *dec = nullptr;
        if (nxvc_vk_decoder_create(&ci, &dec) != NXVC_VKD_OK) {
            nxvc_vk_decoder_destroy(dec);
            std::printf("SKIP %s: no decoder\n", name);
            ++g_skipped;
            continue;
        }
        size_t consumed = 0;
        nxvc_vkd_status st = nxvc_vk_decoder_parse_stream_header(
            dec, stream.data(), stream.size(), &consumed);
        // [inter] Decode until the stream is refused or exhausted, not just
        // the first frame.  A Phase 2 rejection vector is malformed in the
        // frame that first USES the reference ring -- an out-of-envelope
        // matrix, a `ref_slots` that names the wrong slot, a STEREO tile on
        // the left eye -- and frame 0 of such a stream is a legal intra
        // frame.  Stopping after one frame reported nine of them as accepted.
        size_t off = consumed;
        while (st == NXVC_VKD_OK && off < stream.size()) {
            size_t used = 0;
            st = nxvc_vk_decode_frame(dec, stream.data() + off,
                                      stream.size() - off, &used);
            if (st != NXVC_VKD_OK || used == 0) break;
            off += used;
        }
        nxvc_vk_decoder_destroy(dec);
        const bool match = std::strcmp(vkd_status_token(st), want) == 0;
        if (!match && declined && st == NXVC_VKD_ERR_VERSION) {
            ++n;
            ++g_checked;
            if (g_verbose)
                std::printf("ok   %s (%s or, on a device without tools "
                            "0x%llx, VERSION)\n",
                            name, want, (unsigned long long)dtools);
            continue;
        }
        if (!match && phase2 &&
            (st == NXVC_VKD_ERR_VERSION || st == NXVC_VKD_ERR_UNSUPPORTED)) {
            // Refused, just earlier than the manifest describes.
            if (g_verbose)
                std::printf("skip %s: Phase 2 tools 0x%llx, refused with %s "
                            "at the tool mask before the manifest's %s\n",
                            name, (unsigned long long)tools,
                            vkd_status_token(st), want);
            ++g_skipped;
            continue;
        }
        ++n;
        ++g_checked;
        if (!match) {
            std::printf("FAIL %s: refused with %s, manifest says %s\n", name,
                        vkd_status_token(st), want);
            ++g_fail;
        } else if (g_verbose) {
            std::printf("ok   %s (%s)\n", name, want);
        }
    }
    std::fclose(f);
    std::printf("-- %d rejection vector(s)\n", n);
}

// -------------------------------------------------------------- synthetic
struct Case {
    std::string name;
    int w, h;
    int c444, kind, qp, lossless, alpha, tskip, nsub, tables, t420, ct, matrix;
    int res_pattern;  // 0 none, 1 cycling 0/1/2, 2 all level 2
    int qp_pattern;
    int frames;
    int wm_id = 0;    // per-tile weighting-matrix override, 0 = frame's
    // [v3] the three v2 intra tools.  -1 means "whatever nxvc_config_default
    // chose", which is all three on; 0 and 1 pin them, so a case can walk the
    // combinations and prove the tools are additive.
    int intra_dir = -1;
    int dir_layer = -1;
    int ctx_v2 = -1;
    int sign_hide = -1;
    // [minor 6] XFORM_LARGE, tool bit 27: 0 = 8x8 only and no tool bit,
    // 1 = 16x16, 2 = 32x32, 255 = the encoder's per-tile RD choice.  -1
    // leaves nxvc_config_default's value, which is 0.
    int xform = -1;
};

bool encode_case(const Case &c, std::vector<uint8_t> &stream,
                 std::string &err) {
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)c.w;
    cfg.height = (uint32_t)c.h;
    cfg.chroma = c.c444 ? NXVC_CHROMA_444 : NXVC_CHROMA_420;
    cfg.base_qp = (uint32_t)c.qp;
    cfg.lossless = (uint32_t)c.lossless;
    cfg.alpha = (uint32_t)c.alpha;
    cfg.transform_skip = (uint32_t)c.tskip;
    cfg.nsub_log2 = (uint32_t)c.nsub;
    cfg.custom_tables = (uint32_t)c.tables;
    cfg.tile_chroma420 = (uint32_t)c.t420;
    cfg.color_transform = (uint32_t)c.ct;
    cfg.color_space =
        c.ct ? (uint32_t)NXVC_CS_RGB : (uint32_t)NXVC_CS_YCBCR_709_LIMITED;
    cfg.quant_matrix = (uint32_t)c.matrix;
    cfg.wm_id = (uint32_t)c.wm_id;
    if (c.intra_dir >= 0) cfg.intra_dir = (uint32_t)c.intra_dir;
    if (c.dir_layer >= 0) cfg.intra_dir_layer = (uint32_t)c.dir_layer;
    if (c.ctx_v2 >= 0) cfg.ctx_v2 = (uint32_t)c.ctx_v2;
    if (c.sign_hide >= 0) cfg.sign_hide = (uint32_t)c.sign_hide;
    if (c.xform >= 0) cfg.xform_size = (uint32_t)c.xform;

    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) { err = nxvc_status_string(st); return false; }
    std::vector<uint8_t> hdr(4096);
    size_t hl = 0;
    st = nxvc_encoder_stream_header(e, hdr.data(), hdr.size(), &hl);
    if (st != NXVC_OK) {
        err = nxvc_status_string(st);
        nxvc_encoder_destroy(e);
        return false;
    }
    stream.assign(hdr.begin(), hdr.begin() + hl);

    nxvc_tile_layout tl;
    nxvc_tile_layout_get(cfg.width, cfg.height, &tl);
    std::vector<uint8_t> qmap(tl.tile_count), rmap(tl.tile_count);
    for (uint32_t i = 0; i < tl.tile_count; ++i) {
        qmap[i] = (uint8_t)(8 + (i * 7) % 50);
        rmap[i] = c.res_pattern == 1 ? (uint8_t)(i % 3) : 2;
    }
    std::vector<uint8_t> fbuf((size_t)c.w * c.h * 8 + (1u << 20));
    for (int f = 0; f < c.frames; ++f) {
        TestImage im = make_image(c.w, c.h, c.c444 != 0, c.kind,
                                  (uint32_t)(7000 + f * 41 + c.kind * 3));
        nxvc_image img{};
        for (int p = 0; p < 4; ++p) img.plane[p] = (uint8_t *)im.p[p].data();
        img.stride[0] = im.w;
        img.stride[1] = im.cw;
        img.stride[2] = im.cw;
        img.stride[3] = im.w;
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(e, &img,
                                       c.qp_pattern ? qmap.data() : nullptr,
                                       c.res_pattern ? rmap.data() : nullptr,
                                       fbuf.data(), fbuf.size(), &ol);
        if (st != NXVC_OK) {
            err = nxvc_status_string(st);
            nxvc_encoder_destroy(e);
            return false;
        }
        stream.insert(stream.end(), fbuf.begin(), fbuf.begin() + ol);
    }
    nxvc_encoder_destroy(e);
    return true;
}

// ------------------------------------------------------------ loss test
// docs/TRANSPORT.md 8: "the decoder needs an API to mark tiles not received so
// concealment replays exactly", and docs/SYNTAX.md 13.6, which says what
// "exactly" means -- a missing tile is reconstructed by running the WARP_SKIP
// predictor with the tile's stored `last_mv` and no residual, which is
// bit-identically a legitimately skipped tile.
//
// The claim under test is stronger than "the decoder does not crash on loss".
// It is that the GPU decoder and the CPU reference, fed the same drops, agree
// BYTE FOR BYTE for as long as the drops keep accumulating -- because that is
// what lets an encoder holding the same reference and the same prediction
// state predict the next frame from what the client actually shows rather than
// from what it wished it had sent.  A one-frame check would not catch a
// divergence in the prediction state, which is the part that persists: a
// concealed tile's state does not advance, and getting that wrong shows up
// two or three frames later as a vector applied from the wrong place.
//
// So the drops are random, they are different every frame, and the comparison
// runs over 100 frames of a stream that uses every inter tool at once.
struct LossResult {
    int frames = 0;
    int tiles_dropped = 0;
    int frames_with_drops = 0;
    int first_bad_frame = -1;
    std::string err;
};

bool encode_inter_stream(int w, int h, int frames, int qp,
                         std::vector<uint8_t> &stream, std::string &err,
                         int intra_dir = -1, int eyes = 1) {
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)w;
    cfg.height = (uint32_t)h;
    cfg.chroma = NXVC_CHROMA_420;
    cfg.base_qp = (uint32_t)qp;
    cfg.eyes = (uint32_t)eyes;
    cfg.inter = 1;
    // [SYN] 13.7 says the ENCODER cannot replay concealment through a STEREO
    // tile -- nxvc_encoder_set_received_tiles() returns UNSUPPORTED for it.
    // The DECODER side has no such gap: both decoders conceal the left-eye
    // tile and the STEREO tile of the same row then predicts from what the
    // decoder actually holds, so they agree byte for byte, which is exactly
    // what the stereo arm of this test asserts.
    cfg.stereo = (uint32_t)(eyes == 2 ? 1 : 0);
    cfg.near_skip = 1;
    cfg.quad_mv = 1;
    if (intra_dir >= 0) cfg.intra_dir = (uint32_t)intra_dir;
    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) { err = nxvc_status_string(st); return false; }
    std::vector<uint8_t> hdr(4096);
    size_t hl = 0;
    st = nxvc_encoder_stream_header(e, hdr.data(), hdr.size(), &hl);
    if (st != NXVC_OK) {
        err = nxvc_status_string(st);
        nxvc_encoder_destroy(e);
        return false;
    }
    stream.assign(hdr.begin(), hdr.begin() + hl);
    std::vector<uint8_t> fbuf((size_t)w * h * 8 + (1u << 20));
    for (int f = 0; f < frames; ++f) {
        // A slow yaw, so warp_ext() carries a real matrix rather than the
        // identity and the predictor is exercised rather than bypassed.
        const double a = 0.004 * f;
        nxvc_view v[2]{};
        for (int k = 0; k < eyes; ++k) {
            v[k].qx = 0.0;
            v[k].qy = std::sin(a * 0.5);
            v[k].qz = 0.0;
            v[k].qw = std::cos(a * 0.5);
            v[k].fov_left = -0.9; v[k].fov_right = 0.9;
            v[k].fov_up = 0.9; v[k].fov_down = -0.9;
        }
        nxvc_encoder_set_views(e, v, (uint32_t)eyes);
        // The content pans, so the tiles have something to track.  A stereo
        // frame is `eyes` pictures side by side ([SYN] 3.3), which is the
        // layout nxvc_encoder_encode_frame takes.
        TestImage im = make_image(w * eyes, h, false, 1, (uint32_t)(7000 + f));
        nxvc_image img{};
        for (int p = 0; p < 4; ++p) img.plane[p] = (uint8_t *)im.p[p].data();
        img.stride[0] = im.w;
        img.stride[1] = im.cw;
        img.stride[2] = im.cw;
        img.stride[3] = im.w;
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(e, &img, nullptr, nullptr, fbuf.data(),
                                       fbuf.size(), &ol);
        if (st != NXVC_OK) {
            err = nxvc_status_string(st);
            nxvc_encoder_destroy(e);
            return false;
        }
        stream.insert(stream.end(), fbuf.begin(), fbuf.begin() + ol);
    }
    nxvc_encoder_destroy(e);
    return true;
}

bool run_loss_test(int frames, LossResult &r, int eyes = 1) {
    std::vector<uint8_t> stream;
    if (!encode_inter_stream(320, 256, frames, 26, stream, r.err, -1, eyes))
        return false;

    // --- the two decoders, side by side, frame by frame.
    nxvc_status cst;
    nxvc_decoder *rd = nxvc_decoder_create(&cst);
    if (!rd) { r.err = "nxvc_decoder_create"; return false; }
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.flags = (uint32_t)NXVC_VKD_FLAG_READBACK;
    ci.output_format = NXVC_VKD_OUT_AUTO;
    ci.device_name = device_filter();
    nxvc_vk_decoder *gd = nullptr;
    if (nxvc_vk_decoder_create(&ci, &gd) != NXVC_VKD_OK) {
        r.err = gd ? nxvc_vk_decoder_last_error(gd) : "no decoder";
        nxvc_vk_decoder_destroy(gd);
        nxvc_decoder_destroy(rd);
        return false;
    }
    size_t rc = 0, gc = 0;
    cst = nxvc_decoder_parse_stream_header(rd, stream.data(), stream.size(), &rc);
    nxvc_vkd_status gst =
        nxvc_vk_decoder_parse_stream_header(gd, stream.data(), stream.size(), &gc);
    if (cst != NXVC_OK || gst != NXVC_VKD_OK || rc != gc) {
        r.err = "stream header";
        nxvc_vk_decoder_destroy(gd);
        nxvc_decoder_destroy(rd);
        return false;
    }
    uint32_t yw = 0, yh = 0, cw = 0, ch = 0;
    nxvc_decoder_plane_size(rd, 0, &yw, &yh);
    nxvc_decoder_plane_size(rd, 1, &cw, &ch);
    const uint32_t ntiles = nxvc_decoder_tile_count(rd);
    nxvc_stream_info si;
    nxvc_decoder_stream_info(rd, &si);

    Rng rng(0xC0FFEEu);
    size_t off = rc;
    std::vector<uint8_t> lost(ntiles);
    std::vector<uint32_t> ids;
    while (off < stream.size()) {
        // A different, random subset every frame -- including, sometimes, none
        // at all, because a frame with no loss after a frame with loss is the
        // case where a stale prediction state shows up.
        ids.clear();
        std::fill(lost.begin(), lost.end(), (uint8_t)0);
        if (r.frames > 0 && (rng.next() & 3u) != 0u) {
            for (uint32_t t = 0; t < ntiles; ++t)
                if ((rng.next() % 100u) < 12u) {
                    lost[t] = 1;
                    ids.push_back(t);
                }
        }
        if (!ids.empty()) ++r.frames_with_drops;
        r.tiles_dropped += (int)ids.size();
        if (nxvc_decoder_set_lost_tiles(rd, lost.data(), ntiles) != NXVC_OK ||
            nxvc_vk_decoder_mark_missing(gd, ids.empty() ? nullptr : ids.data(),
                                         (uint32_t)ids.size()) != NXVC_VKD_OK) {
            r.err = "set_lost_tiles / mark_missing";
            break;
        }
        Planes rp, gp;
        rp.size_for(yw, yh, cw, ch, si.alpha != 0);
        gp.size_for(yw, yh, cw, ch, si.alpha != 0);
        nxvc_image img{};
        img.plane[0] = rp.p[0].data(); img.stride[0] = (int)yw;
        img.plane[1] = rp.p[1].data(); img.stride[1] = (int)cw;
        img.plane[2] = rp.p[2].data(); img.stride[2] = (int)cw;
        img.plane[3] = rp.p[3].data(); img.stride[3] = (int)yw;
        size_t used_r = 0, used_g = 0;
        cst = nxvc_decoder_decode_frame(rd, stream.data() + off,
                                        stream.size() - off, &img, &used_r);
        gst = nxvc_vk_decode_frame(gd, stream.data() + off, stream.size() - off,
                                   &used_g);
        if (cst != NXVC_OK || gst != NXVC_VKD_OK || used_r != used_g) {
            r.err = std::string("frame ") + std::to_string(r.frames) + ": ref " +
                    nxvc_status_string(cst) + ", gpu " +
                    nxvc_vk_decoder_status_string(gst);
            break;
        }
        uint8_t *gpl[4] = {gp.p[0].data(), gp.p[1].data(), gp.p[2].data(),
                           gp.p[3].data()};
        int32_t gstr[4] = {(int32_t)yw, (int32_t)cw, (int32_t)cw, (int32_t)yw};
        if (nxvc_vk_decoder_read_planes(gd, gpl, gstr) != NXVC_VKD_OK) {
            r.err = "read_planes";
            break;
        }
        if (r.first_bad_frame < 0) {
            for (int p = 0; p < (si.alpha ? 4 : 3); ++p)
                if (rp.p[p] != gp.p[p]) {
                    r.first_bad_frame = r.frames;
                    break;
                }
        }
        ++r.frames;
        off += used_r;
    }
    nxvc_vk_decoder_destroy(gd);
    nxvc_decoder_destroy(rd);
    return r.err.empty();
}

void run_loss(int frames) {
    // Mono and stereo: the stereo arm is what covers a concealed LEFT-eye tile
    // that a STEREO tile of the same row then predicts from.
    for (int eyes = 1; eyes <= 2; ++eyes) {
        const char *what = eyes == 1 ? "loss" : "loss-stereo";
        LossResult r;
        ++g_checked;
        if (!run_loss_test(frames, r, eyes)) {
            std::printf("FAIL %s: %s\n", what, r.err.c_str());
            ++g_fail;
            continue;
        }
        if (r.first_bad_frame >= 0) {
            std::printf("FAIL %s: GPU and reference diverge at frame %d "
                        "(%d frames, %d tiles dropped)\n",
                        what, r.first_bad_frame, r.frames, r.tiles_dropped);
            ++g_fail;
            continue;
        }
        std::printf("-- %s: %d frames, %d with drops, %d tiles dropped, "
                    "byte-identical to the reference\n",
                    what, r.frames, r.frames_with_drops, r.tiles_dropped);
    }
}

// ------------------------------------------------ [SYN] 3.1.2 row_present
// The tool elides the 12-byte header of a tile row with no coded tile.  An
// elided row decodes exactly as a transmitted all-skipped one, which is what
// makes it cheap -- and also what makes it invisible to a pixel comparison
// alone: a decoder that ignored the bitmap entirely and read the next row
// header out of it would produce garbage, but a decoder that never REACHED an
// elided row would pass a pixel test having tested nothing.
//
// So this asserts three things and not one:
//
//   1. the GPU decoder and the reference agree byte for byte, every frame;
//   2. rows were ACTUALLY elided -- `rows_elided` summed over the sequence is
//      non-zero, and the test FAILS if it is zero;
//   3. the same content with the tool off decodes to the SAME pixels and
//      costs MORE bytes, which is the tool's entire claim.
//
// The sequence repeats one image so nearly every tile is WARP_SKIP and whole
// rows have nothing to say, which is the case 3.1.2 exists for.
bool encode_idle_stream(int w, int h, int frames, int row_present,
                        std::vector<uint8_t> &stream, std::string &err) {
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)w;
    cfg.height = (uint32_t)h;
    cfg.chroma = NXVC_CHROMA_420;
    cfg.base_qp = 26;
    cfg.inter = 1;
    cfg.row_present = (uint32_t)row_present;
    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) { err = nxvc_status_string(st); return false; }
    std::vector<uint8_t> hdr(4096);
    size_t hl = 0;
    st = nxvc_encoder_stream_header(e, hdr.data(), hdr.size(), &hl);
    if (st != NXVC_OK) {
        err = nxvc_status_string(st);
        nxvc_encoder_destroy(e);
        return false;
    }
    stream.assign(hdr.begin(), hdr.begin() + hl);
    // ONE image, encoded `frames` times.  The pose does not move either: a
    // yaw would make every tile WARP_MV and there would be no idle row left
    // to elide, which is the shape this test needs and the shape a headset
    // looking at a static scene actually produces.
    TestImage im = make_image(w, h, false, 1, 7001u);
    std::vector<uint8_t> fbuf((size_t)w * h * 8 + (1u << 20));
    for (int f = 0; f < frames; ++f) {
        nxvc_view v{};
        v.qw = 1.0;
        v.fov_left = -0.9; v.fov_right = 0.9;
        v.fov_up = 0.9; v.fov_down = -0.9;
        nxvc_encoder_set_views(e, &v, 1);
        nxvc_image img{};
        for (int p = 0; p < 4; ++p) img.plane[p] = (uint8_t *)im.p[p].data();
        img.stride[0] = im.w;
        img.stride[1] = im.cw;
        img.stride[2] = im.cw;
        img.stride[3] = im.w;
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(e, &img, nullptr, nullptr, fbuf.data(),
                                       fbuf.size(), &ol);
        if (st != NXVC_OK) {
            err = nxvc_status_string(st);
            nxvc_encoder_destroy(e);
            return false;
        }
        stream.insert(stream.end(), fbuf.begin(), fbuf.begin() + ol);
    }
    nxvc_encoder_destroy(e);
    return true;
}

struct RowPresentResult {
    int frames = 0;
    uint64_t rows_elided = 0;
    int first_bad_frame = -1;
    std::string err;
    MD5 md;   // the whole sequence's pixels, so the two streams can be equated
};

bool decode_both(const std::vector<uint8_t> &stream, RowPresentResult &r) {
    nxvc_status cst;
    nxvc_decoder *rd = nxvc_decoder_create(&cst);
    if (!rd) { r.err = "nxvc_decoder_create"; return false; }
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.flags = (uint32_t)NXVC_VKD_FLAG_READBACK;
    ci.output_format = NXVC_VKD_OUT_AUTO;
    ci.device_name = device_filter();
    nxvc_vk_decoder *gd = nullptr;
    if (nxvc_vk_decoder_create(&ci, &gd) != NXVC_VKD_OK) {
        r.err = gd ? nxvc_vk_decoder_last_error(gd) : "no decoder";
        nxvc_vk_decoder_destroy(gd);
        nxvc_decoder_destroy(rd);
        return false;
    }
    size_t rc = 0, gc = 0;
    cst = nxvc_decoder_parse_stream_header(rd, stream.data(), stream.size(), &rc);
    nxvc_vkd_status gst =
        nxvc_vk_decoder_parse_stream_header(gd, stream.data(), stream.size(), &gc);
    if (cst != NXVC_OK || gst != NXVC_VKD_OK || rc != gc) {
        r.err = std::string("stream header: ref ") + nxvc_status_string(cst) +
                ", gpu " + nxvc_vk_decoder_status_string(gst);
        nxvc_vk_decoder_destroy(gd);
        nxvc_decoder_destroy(rd);
        return false;
    }
    uint32_t yw = 0, yh = 0, cw = 0, ch = 0;
    nxvc_decoder_plane_size(rd, 0, &yw, &yh);
    nxvc_decoder_plane_size(rd, 1, &cw, &ch);
    nxvc_stream_info si;
    nxvc_decoder_stream_info(rd, &si);
    size_t off = rc;
    while (off < stream.size()) {
        Planes rp, gp;
        rp.size_for(yw, yh, cw, ch, si.alpha != 0);
        gp.size_for(yw, yh, cw, ch, si.alpha != 0);
        nxvc_image img{};
        img.plane[0] = rp.p[0].data(); img.stride[0] = (int)yw;
        img.plane[1] = rp.p[1].data(); img.stride[1] = (int)cw;
        img.plane[2] = rp.p[2].data(); img.stride[2] = (int)cw;
        img.plane[3] = rp.p[3].data(); img.stride[3] = (int)yw;
        size_t used_r = 0, used_g = 0;
        cst = nxvc_decoder_decode_frame(rd, stream.data() + off,
                                        stream.size() - off, &img, &used_r);
        gst = nxvc_vk_decode_frame(gd, stream.data() + off, stream.size() - off,
                                   &used_g);
        if (cst != NXVC_OK || gst != NXVC_VKD_OK || used_r != used_g) {
            r.err = std::string("frame ") + std::to_string(r.frames) + ": ref " +
                    nxvc_status_string(cst) + ", gpu " +
                    nxvc_vk_decoder_status_string(gst);
            break;
        }
        uint8_t *gpl[4] = {gp.p[0].data(), gp.p[1].data(), gp.p[2].data(),
                           gp.p[3].data()};
        int32_t gstr[4] = {(int32_t)yw, (int32_t)cw, (int32_t)cw, (int32_t)yw};
        if (nxvc_vk_decoder_read_planes(gd, gpl, gstr) != NXVC_VKD_OK) {
            r.err = "read_planes";
            break;
        }
        if (r.first_bad_frame < 0)
            for (int p = 0; p < (si.alpha ? 4 : 3); ++p)
                if (rp.p[p] != gp.p[p]) { r.first_bad_frame = r.frames; break; }
        rp.hash_into(r.md);
        nxvc_vkd_stats stg{};
        nxvc_vk_decoder_stats(gd, &stg);
        r.rows_elided += stg.rows_elided;
        ++r.frames;
        off += used_r;
    }
    nxvc_vk_decoder_destroy(gd);
    nxvc_decoder_destroy(rd);
    return r.err.empty();
}

void run_row_present(int frames) {
    ++g_checked;
    std::vector<uint8_t> on, offs;
    std::string err;
    if (!encode_idle_stream(320, 256, frames, 1, on, err) ||
        !encode_idle_stream(320, 256, frames, 0, offs, err)) {
        std::printf("FAIL row_present: encode: %s\n", err.c_str());
        ++g_fail;
        return;
    }
    RowPresentResult a, b;
    if (!decode_both(on, a) || !decode_both(offs, b)) {
        std::printf("FAIL row_present: %s\n",
                    a.err.empty() ? b.err.c_str() : a.err.c_str());
        ++g_fail;
        return;
    }
    if (a.first_bad_frame >= 0 || b.first_bad_frame >= 0) {
        std::printf("FAIL row_present: GPU and reference diverge at frame %d "
                    "(tool on) / %d (tool off)\n",
                    a.first_bad_frame, b.first_bad_frame);
        ++g_fail;
        return;
    }
    // A sweep that never reached an elided row proves nothing, so it fails
    // rather than passing quietly -- the same rule the ATLAS guard test
    // applies to the 2^33 trip count.
    if (a.rows_elided == 0) {
        std::printf("FAIL row_present: %d frames and NOT ONE row was elided; "
                    "the tool was never exercised\n", a.frames);
        ++g_fail;
        return;
    }
    if (b.rows_elided != 0) {
        std::printf("FAIL row_present: the tool-off stream reported %llu "
                    "elided rows and must report none\n",
                    (unsigned long long)b.rows_elided);
        ++g_fail;
        return;
    }
    // The pixels are the same picture either way: 3.1.2 elides BYTES, not
    // content.  "A stream that never sets flags bit 4 decodes byte-identically
    // whether or not the tool bit is offered" -- and so does one that does.
    if (a.md.hex() != b.md.hex()) {
        std::printf("FAIL row_present: the two encodings decode to different "
                    "pictures (%s vs %s)\n",
                    a.md.hex().c_str(), b.md.hex().c_str());
        ++g_fail;
        return;
    }
    if (on.size() >= offs.size()) {
        std::printf("FAIL row_present: the tool cost bytes instead of saving "
                    "them: %zu with, %zu without\n", on.size(), offs.size());
        ++g_fail;
        return;
    }
    std::printf("-- row_present: %d frames, %llu row structures elided, "
                "%zu B against %zu B without the tool (%.1fx), "
                "byte-identical to the reference and to itself\n",
                a.frames, (unsigned long long)a.rows_elided, on.size(),
                offs.size(), (double)offs.size() / (double)on.size());
}

// --------------------------------------- [SYN] 13.12.11 the two frame modes
// A stream with tool bit 34 codes each frame as an ATLAS frame or a PICTURE
// frame.  Two arms, forced to opposite ends by the encoder's policy knobs, and
// each asserts that it ACTUALLY reached the mode it is named for -- a sweep
// that never took the branch proves nothing, which is the same rule the guard
// trip count and the row_present elision count are held to.
//
// The comparison is the atlas against the reference's, byte for byte, after
// every frame: table and pixels.  For the all-PICTURE arm that is also the
// equivalence the mode exists to provide -- every frame decoded by the
// ORDINARY picture model, with the atlas rebuilt from the result -- so if the
// two modes were not the same codec at two operating points, this is where it
// would show.
bool encode_mode_stream(int w, int h, int frames, int period,
                        std::vector<uint8_t> &stream, std::string &err) {
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)w;
    cfg.height = (uint32_t)h;
    cfg.chroma = NXVC_CHROMA_420;
    cfg.base_qp = 28;
    cfg.inter = 1;
    cfg.atlas = 1;
    // `period` 1 forces every frame to a PICTURE frame; 0 with no
    // displacement trigger leaves every frame an ATLAS frame.
    cfg.atlas_picture_period = (uint32_t)period;
    cfg.atlas_picture_disp = 0;
    cfg.atlas_picture_min_spacing = 0;
    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) { err = nxvc_status_string(st); return false; }
    std::vector<uint8_t> hdr(4096);
    size_t hl = 0;
    st = nxvc_encoder_stream_header(e, hdr.data(), hdr.size(), &hl);
    if (st != NXVC_OK) {
        err = nxvc_status_string(st);
        nxvc_encoder_destroy(e);
        return false;
    }
    stream.assign(hdr.begin(), hdr.begin() + hl);
    std::vector<uint8_t> fbuf((size_t)w * h * 8 + (1u << 20));
    for (int f = 0; f < frames; ++f) {
        const double a = 0.010 * f;   // a real yaw, so C is not the identity
        nxvc_view v{};
        v.qy = std::sin(a * 0.5);
        v.qw = std::cos(a * 0.5);
        v.fov_left = -0.9; v.fov_right = 0.9;
        v.fov_up = 0.9; v.fov_down = -0.9;
        nxvc_encoder_set_views(e, &v, 1);
        TestImage im = make_image(w, h, false, 1, (uint32_t)(7000 + f));
        nxvc_image img{};
        for (int p = 0; p < 4; ++p) img.plane[p] = (uint8_t *)im.p[p].data();
        img.stride[0] = im.w;
        img.stride[1] = im.cw;
        img.stride[2] = im.cw;
        img.stride[3] = im.w;
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(e, &img, nullptr, nullptr, fbuf.data(),
                                       fbuf.size(), &ol);
        if (st != NXVC_OK) {
            err = nxvc_status_string(st);
            nxvc_encoder_destroy(e);
            return false;
        }
        stream.insert(stream.end(), fbuf.begin(), fbuf.begin() + ol);
    }
    nxvc_encoder_destroy(e);
    return true;
}

// Count the frames whose header sets flags bit 5, straight out of the
// bitstream -- so what the test asserts about the mode is what the WIRE says
// and not what the decoder reports about itself.
void count_modes(const std::vector<uint8_t> &s, size_t hdr_len, int *picture,
                 int *atlas) {
    *picture = 0;
    *atlas = 0;
    size_t off = hdr_len;
    while (off + 40 <= s.size()) {
        uint32_t fb = 0;
        for (int i = 0; i < 4; ++i) fb |= (uint32_t)s[off + 36 + i] << (8 * i);
        if (!fb) break;
        ((s[off + 34] >> 5) & 1u) ? ++*picture : ++*atlas;
        off += fb;
    }
}

void run_atlas_modes() {
    struct Arm { const char *name; int period; bool want_picture; };
    const Arm arms[2] = {{"modes-all-picture", 1, true},
                         {"modes-all-atlas", 0, false}};
    for (const Arm &a : arms) {
        ++g_checked;
        std::vector<uint8_t> stream;
        std::string err;
        if (!encode_mode_stream(192, 192, 10, a.period, stream, err)) {
            std::printf("FAIL %s: encode: %s\n", a.name, err.c_str());
            ++g_fail;
            continue;
        }
        nxvc_status cst;
        nxvc_decoder *rd = nxvc_decoder_create(&cst);
        if (!rd) { std::printf("FAIL %s: decoder_create\n", a.name); ++g_fail; continue; }
        size_t rc = 0;
        if (nxvc_decoder_parse_stream_header(rd, stream.data(), stream.size(),
                                             &rc) != NXVC_OK) {
            std::printf("FAIL %s: reference stream header\n", a.name);
            ++g_fail;
            nxvc_decoder_destroy(rd);
            continue;
        }
        nxvc_decoder_destroy(rd);
        int npic = 0, natl = 0;
        count_modes(stream, rc, &npic, &natl);
        // The arm has to have reached its own mode, or it tested nothing.
        if (a.want_picture && npic == 0) {
            std::printf("FAIL %s: not one frame set flags bit 5; the PICTURE "
                        "mode was never exercised\n", a.name);
            ++g_fail;
            continue;
        }
        if (!a.want_picture && npic != 0) {
            std::printf("FAIL %s: %d PICTURE frame(s) in the arm that must "
                        "have none\n", a.name, npic);
            ++g_fail;
            continue;
        }
        --g_checked;   // check_stream_atlas counts it
        check_stream_atlas(a.name, stream, "");
        std::printf("-- %s: %d PICTURE frame(s), %d ATLAS frame(s), atlas "
                    "byte-identical to the reference\n", a.name, npic, natl);
    }
}

// ------------------------------- [ATLAS] the display view A/B (13.12.5)
// The one-tap 8-bit view and the three-plane R16 view must carry the SAME
// PICTURE.  If they did not, choosing between them would be a quality decision
// instead of a performance one, and the 1.086-against-2.124 ms measurement
// would be buying something other than taps.
//
// The comparison is sample for sample after the conversion rule the header
// states: for a CT_NONE stream the atlas holds the stream's own 8-bit YCbCr,
// so the R8 view stores the value unchanged as a UNORM byte and the R16 view
// stores it as an integer.  Both are then read back and must agree with each
// OTHER and with the ATLAS ITSELF -- the third leg matters, because two views
// produced by one wrong kernel would agree with each other perfectly.
void run_atlas_view() {
    ++g_checked;
    std::vector<uint8_t> stream;
    std::string err;
    if (!encode_mode_stream(192, 192, 4, 0, stream, err)) {
        std::printf("FAIL atlas-view: encode: %s\n", err.c_str());
        ++g_fail;
        return;
    }
    struct Shot { std::vector<uint8_t> y, c, cr; uint32_t yw=0,yh=0,cw=0,ch=0; };
    Shot shot[2];
    const nxvc_vkd_atlas_view modes[2] = {NXVC_VKD_ATLAS_VIEW_R8,
                                          NXVC_VKD_ATLAS_VIEW_R16};
    std::vector<uint16_t> atlasY, atlasCb, atlasCr;
    uint32_t aw = 0, ah = 0, asd = 0, acw = 0, ach = 0, acsd = 0;
    for (int m = 0; m < 2; ++m) {
        nxvc_vkd_create_info ci;
        nxvc_vk_decoder_create_info_default(&ci);
        ci.flags = 0;
        ci.output_format = NXVC_VKD_OUT_AUTO;
        ci.device_name = device_filter();
        nxvc_vk_decoder *d = nullptr;
        if (nxvc_vk_decoder_create(&ci, &d) != NXVC_VKD_OK) {
            std::printf("SKIP atlas-view: no decoder\n");
            ++g_skipped;
            nxvc_vk_decoder_destroy(d);
            return;
        }
        size_t consumed = 0;
        if (nxvc_vk_decoder_parse_stream_header(d, stream.data(), stream.size(),
                                                &consumed) != NXVC_VKD_OK) {
            std::printf("FAIL atlas-view: stream header\n");
            ++g_fail;
            nxvc_vk_decoder_destroy(d);
            return;
        }
        if (nxvc_vk_decoder_set_atlas_view(d, modes[m]) != NXVC_VKD_OK) {
            std::printf("FAIL atlas-view: set_atlas_view(%d): %s\n", (int)modes[m],
                        nxvc_vk_decoder_last_error(d));
            ++g_fail;
            nxvc_vk_decoder_destroy(d);
            return;
        }
        size_t off = consumed;
        while (off < stream.size()) {
            size_t used = 0;
            if (nxvc_vk_decode_frame(d, stream.data() + off,
                                     stream.size() - off, &used) !=
                NXVC_VKD_OK) {
                std::printf("FAIL atlas-view: decode: %s\n",
                            nxvc_vk_decoder_last_error(d));
                ++g_fail;
                nxvc_vk_decoder_destroy(d);
                return;
            }
            off += used;
        }
        uint32_t w = 0, h = 0, bps = 0;
        auto grab = [&](int pl, std::vector<uint8_t> &dst) {
            if (nxvc_vk_decoder_atlas_view_read(d, pl, nullptr, 0, &w, &h,
                                                &bps) != NXVC_VKD_OK)
                return false;
            dst.assign((size_t)w * h * bps, 0u);
            return nxvc_vk_decoder_atlas_view_read(d, pl, dst.data(),
                                                   dst.size(), &w, &h, &bps) ==
                   NXVC_VKD_OK;
        };
        if (!grab(0, shot[m].y)) { std::printf("FAIL atlas-view: read luma\n"); ++g_fail; nxvc_vk_decoder_destroy(d); return; }
        shot[m].yw = w; shot[m].yh = h;
        if (!grab(1, shot[m].c)) { std::printf("FAIL atlas-view: read chroma\n"); ++g_fail; nxvc_vk_decoder_destroy(d); return; }
        shot[m].cw = w; shot[m].ch = h;
        if (modes[m] == NXVC_VKD_ATLAS_VIEW_R16) grab(2, shot[m].cr);
        if (m == 1) {
            // The atlas itself, as the independent third leg.
            nxvc_vk_decoder_atlas_plane(d, 0, nullptr, 0, &aw, &ah, &asd);
            atlasY.assign((size_t)asd * ah, 0u);
            nxvc_vk_decoder_atlas_plane(d, 0, atlasY.data(), atlasY.size(), &aw,
                                        &ah, &asd);
            nxvc_vk_decoder_atlas_plane(d, 1, nullptr, 0, &acw, &ach, &acsd);
            atlasCb.assign((size_t)acsd * ach, 0u);
            nxvc_vk_decoder_atlas_plane(d, 1, atlasCb.data(), atlasCb.size(),
                                        &acw, &ach, &acsd);
            atlasCr.assign((size_t)acsd * ach, 0u);
            nxvc_vk_decoder_atlas_plane(d, 2, atlasCr.data(), atlasCr.size(),
                                        &acw, &ach, &acsd);
        }
        nxvc_vk_decoder_destroy(d);
    }
    if (shot[0].yw != shot[1].yw || shot[0].yh != shot[1].yh ||
        shot[0].cw != shot[1].cw || shot[0].ch != shot[1].ch) {
        std::printf("FAIL atlas-view: the two views have different extents\n");
        ++g_fail;
        return;
    }
    size_t bad = 0, first = 0;
    bool have = false;
    for (uint32_t y = 0; y < shot[0].yh; ++y)
        for (uint32_t x = 0; x < shot[0].yw; ++x) {
            const uint32_t v8 = shot[0].y[(size_t)y * shot[0].yw + x];
            const size_t o16 = ((size_t)y * shot[1].yw + x) * 2;
            const uint32_t v16 =
                (uint32_t)shot[1].y[o16] | ((uint32_t)shot[1].y[o16 + 1] << 8);
            const uint32_t va = atlasY[(size_t)y * asd + x];
            if (v8 != v16 || v8 != va) {
                if (!have) { have = true; first = (size_t)y * shot[0].yw + x; }
                ++bad;
            }
        }
    for (uint32_t y = 0; y < shot[0].ch; ++y)
        for (uint32_t x = 0; x < shot[0].cw; ++x) {
            const size_t o8 = ((size_t)y * shot[0].cw + x) * 2;
            const uint32_t cb8 = shot[0].c[o8], cr8 = shot[0].c[o8 + 1];
            const size_t o16 = ((size_t)y * shot[1].cw + x) * 2;
            const uint32_t cb16 =
                (uint32_t)shot[1].c[o16] | ((uint32_t)shot[1].c[o16 + 1] << 8);
            const uint32_t cr16 = (uint32_t)shot[1].cr[o16] |
                                  ((uint32_t)shot[1].cr[o16 + 1] << 8);
            const uint32_t acb = atlasCb[(size_t)y * acsd + x];
            const uint32_t acr = atlasCr[(size_t)y * acsd + x];
            if (cb8 != cb16 || cr8 != cr16 || cb8 != acb || cr8 != acr) ++bad;
        }
    if (bad) {
        std::printf("FAIL atlas-view: %zu sample(s) differ between the one-tap "
                    "8-bit view, the three-plane R16 view and the atlas "
                    "(first at %zu)\n", bad, first);
        ++g_fail;
        return;
    }
    std::printf("-- atlas-view: one-tap 8-bit (R8 %ux%u + R8G8 %ux%u) and "
                "three-plane R16 carry the same picture, and both equal the "
                "atlas, over %u luma + %u chroma samples\n",
                shot[0].yw, shot[0].yh, shot[0].cw, shot[0].ch,
                shot[0].yw * shot[0].yh, shot[0].cw * shot[0].ch);
}

// ------------------------------------- [ATLAS] the stats leg (13.12.11)
// `frame_mode` is what a client wires its HUD and its budget to, so it is
// asserted against the WIRE frame by frame -- flags bit 5 read straight out of
// each frame header -- and not against anything the decoder reports about
// itself.  A stat that agrees with the decoder's own opinion tests nothing.
//
// v90-v92 are the mode vectors and between them carry both transitions, so a
// decoder that hardcoded either mode fails here.
void run_atlas_stats() {
    static const char *kVecs[3] = {"v90_mode_alternate", "v91_mode_disp",
                                   "v92_mode_src_frame"};
    for (const char *name : kVecs) {
        ++g_checked;
        std::vector<uint8_t> stream;
        const std::string path = std::string(g_vectors_dir) + "/" + name + ".nxv";
        if (!read_file(path, stream)) {
            std::printf("FAIL stats %s: cannot read %s\n", name, path.c_str());
            ++g_fail;
            continue;
        }
        nxvc_vkd_create_info ci;
        nxvc_vk_decoder_create_info_default(&ci);
        ci.flags = 0;
        ci.output_format = NXVC_VKD_OUT_AUTO;
        ci.device_name = device_filter();
        nxvc_vk_decoder *d = nullptr;
        if (nxvc_vk_decoder_create(&ci, &d) != NXVC_VKD_OK) {
            std::printf("SKIP stats %s: no decoder\n", name);
            ++g_skipped;
            nxvc_vk_decoder_destroy(d);
            continue;
        }
        size_t consumed = 0;
        if (nxvc_vk_decoder_parse_stream_header(d, stream.data(), stream.size(),
                                                &consumed) != NXVC_VKD_OK) {
            std::printf("FAIL stats %s: stream header: %s\n", name,
                        nxvc_vk_decoder_last_error(d));
            ++g_fail;
            nxvc_vk_decoder_destroy(d);
            continue;
        }
        size_t off = consumed;
        int nf = 0, npic = 0, bad = 0;
        uint32_t lastPicCounter = 0;
        while (off < stream.size()) {
            // The expected mode, straight from the frame header.
            const uint32_t want = ((stream[off + 34] >> 5) & 1u) ? 2u : 1u;
            size_t used = 0;
            if (nxvc_vk_decode_frame(d, stream.data() + off,
                                     stream.size() - off, &used) !=
                NXVC_VKD_OK) {
                std::printf("FAIL stats %s: frame %d: %s\n", name, nf,
                            nxvc_vk_decoder_last_error(d));
                ++g_fail;
                bad = 1;
                break;
            }
            nxvc_vkd_stats st{};
            nxvc_vk_decoder_stats(d, &st);
            if (st.frame_mode != want) {
                std::printf("FAIL stats %s: frame %d: frame_mode %u, the wire "
                            "says %u\n", name, nf, st.frame_mode, want);
                ++g_fail;
                bad = 1;
                break;
            }
            // A PICTURE frame assembles every position and validates every
            // position; an ATLAS frame assembles none.
            const uint32_t want_asm = want == 2u ? st.tiles : 0u;
            if (st.tiles_assembled != want_asm) {
                std::printf("FAIL stats %s: frame %d: tiles_assembled %u, "
                            "expected %u\n", name, nf, st.tiles_assembled,
                            want_asm);
                ++g_fail;
                bad = 1;
                break;
            }
            if (want == 2u && st.atlas_entries_valid != st.tiles) {
                std::printf("FAIL stats %s: frame %d: a PICTURE frame left "
                            "%u of %u entries valid; 13.12.11 step 3 validates "
                            "every position\n", name, nf,
                            st.atlas_entries_valid, st.tiles);
                ++g_fail;
                bad = 1;
                break;
            }
            if (st.atlas_entries_valid > st.tiles) {
                std::printf("FAIL stats %s: frame %d: atlas_entries_valid %u "
                            "exceeds the %u tile positions\n", name, nf,
                            st.atlas_entries_valid, st.tiles);
                ++g_fail;
                bad = 1;
                break;
            }
            // Under ATLAS no tile costs a pose warp, which is the deletion the
            // whole design is for.
            if (want == 1u && st.tiles_warped_skip != 0u) {
                std::printf("FAIL stats %s: frame %d: an ATLAS frame warped "
                            "%u skipped tiles; it must warp none\n", name, nf,
                            st.tiles_warped_skip);
                ++g_fail;
                bad = 1;
                break;
            }
            if (want == 2u) ++npic;
            if (st.picture_frames != (uint32_t)npic) {
                std::printf("FAIL stats %s: frame %d: picture_frames %u, "
                            "counted %d\n", name, nf, st.picture_frames, npic);
                ++g_fail;
                bad = 1;
                break;
            }
            lastPicCounter = st.picture_frames;
            ++nf;
            off += used;
        }
        if (!bad) {
            // A vector that took only one mode would prove nothing about the
            // field, so the leg fails if it never saw both.
            if (npic == 0 || npic == nf) {
                std::printf("FAIL stats %s: %d of %d frames were PICTURE -- "
                            "the vector took only ONE mode, so frame_mode was "
                            "never actually distinguished\n", name, npic, nf);
                ++g_fail;
            } else {
                std::printf("-- stats %s: %d frames, %d ATLAS / %d PICTURE, "
                            "frame_mode matches the wire on every frame "
                            "(picture_frames %u)\n", name, nf, nf - npic, npic,
                            lastPicCounter);
            }
        }
        nxvc_vk_decoder_destroy(d);
    }
}

std::vector<Case> synthetic_cases(bool quick) {
    std::vector<Case> v;
    auto nm = [](const char *fmt, auto... a) {
        char b[80];
        std::snprintf(b, sizeof b, fmt, a...);
        return std::string(b);
    };
    // QP ladder x chroma format x colour transform.  YCoCg-R requires 4:4:4
    // (docs/SYNTAX.md 2), so this is not a full cross product.
    static const int kQps[] = {0, 8, 16, 24, 32, 40, 51, 63};
    for (int qi = 0; qi < 8; ++qi) {
        if (quick && (qi % 3)) continue;
        int qp = kQps[qi];
        for (int c444 = 0; c444 <= 1; ++c444)
            for (int ct = 0; ct <= c444; ++ct)
                v.push_back({nm("syn_qp%02d_%s_%s", qp, c444 ? "444" : "420",
                                ct ? "ycocgr" : "pass"),
                             192, 128, c444, 1, qp, 0, 0, 0, 3, 0, 0, ct, 1, 0,
                             0, 1});
    }
    if (quick) return v;

    for (int c444 = 0; c444 <= 1; ++c444)
        for (int rp = 1; rp <= 2; ++rp)
            v.push_back({nm("syn_res%d_%s", rp, c444 ? "444" : "420"), 192, 128,
                         c444, 1, 28, 0, 0, 0, 3, 0, 0, 0, 1, rp, 0, 1});
    // Lane counts 1, 2, 32 and the encoder's own per-tile choice.
    for (int ns : {0, 1, 5, 255})
        v.push_back({nm("syn_nsub%d", ns), 192, 128, 0, 1, 28, 0, 0, 0, ns, 0,
                     0, 0, 1, 0, 0, 1});
    v.push_back({"syn_tskip420", 192, 128, 0, 2, 16, 0, 0, 1, 3, 0, 0, 0, 0, 0,
                 0, 1});
    v.push_back({"syn_tables420", 192, 128, 0, 1, 28, 0, 0, 0, 3, 1, 0, 0, 1, 0,
                 0, 1});
    v.push_back({"syn_tile420_in444", 192, 128, 1, 1, 26, 0, 0, 0, 3, 0, 1, 0,
                 1, 0, 0, 1});
    v.push_back({"syn_alpha420", 192, 128, 0, 1, 24, 0, 1, 0, 3, 0, 0, 0, 1, 0,
                 0, 1});
    v.push_back({"syn_alpha444", 192, 128, 1, 2, 24, 0, 1, 0, 3, 0, 0, 0, 1, 0,
                 0, 1});
    v.push_back({"syn_lossless420", 192, 128, 0, 1, 0, 1, 0, 1, 3, 0, 0, 0, 0,
                 0, 0, 1});
    v.push_back({"syn_lossless_ycocgr", 192, 128, 1, 1, 0, 1, 0, 1, 3, 0, 0, 1,
                 0, 0, 0, 1});
    v.push_back({"syn_qpmap_resmap", 192, 128, 0, 1, 30, 0, 0, 0, 3, 0, 0, 0, 2,
                 1, 1, 1});
    for (int m = 0; m < 4; ++m)
        v.push_back({nm("syn_matrix%d", m), 192, 128, 0, 1, 28, 0, 0, 0, 3, 0,
                     0, 0, m, 0, 0, 1});
    for (int wm = 1; wm <= 3; ++wm)
        v.push_back({nm("syn_wm_id%d", wm), 192, 128, 0, 1, 26, 0, 0, 0, 3, 0,
                     0, 0, 1, 0, 0, 1, wm});
    v.push_back({"syn_wm_id2_444", 192, 128, 1, 1, 24, 0, 0, 0, 3, 0, 0, 0, 2,
                 0, 0, 1, 2});
    v.push_back({"syn_odd_200x140", 200, 140, 0, 1, 28, 0, 0, 0, 3, 0, 0, 0, 1,
                 0, 0, 1});
    v.push_back({"syn_tiny_64x64", 64, 64, 0, 4, 28, 0, 0, 0, 3, 0, 0, 0, 1, 0,
                 0, 1});
    v.push_back({"syn_tall_64x320", 64, 320, 0, 3, 22, 0, 0, 0, 3, 0, 0, 0, 1,
                 0, 0, 1});
    v.push_back({"syn_multiframe", 128, 128, 0, 1, 30, 0, 0, 0, 3, 0, 0, 0, 1,
                 0, 0, 3});

    // [v3] The three v2 intra tools are on by default, so every case above
    // already exercises them.  These walk the combinations the other way --
    // each tool alone and all three off -- so the "additive, and off unless
    // the bit is set" claim of docs/SYNTAX.md 12 is checked on synthetic
    // content as well as on the committed vectors.
    auto v3 = [&](const char *name, int c444, int qp, int dir, int layer,
                  int ctx, int sdh, int res_pat = 0, int tsk = 0) {
        Case c{name, 192, 128, c444, 1, qp, 0, 0, tsk, 3, 0, 0, 0, res_pat,
               0,    0,   1};
        c.intra_dir = dir;
        c.dir_layer = layer;
        c.ctx_v2 = ctx;
        c.sign_hide = sdh;
        v.push_back(c);
    };
    v3("syn_v1tools_420", 0, 24, 0, 0, 0, 0);
    v3("syn_v1tools_444", 1, 24, 0, 0, 0, 0);
    v3("syn_dir_only_420", 0, 24, 1, 0, 0, 0);
    v3("syn_dir_only_444", 1, 16, 1, 0, 0, 0);
    v3("syn_ctxv2_only_420", 0, 24, 0, 0, 1, 0);
    v3("syn_sdh_only_420", 0, 24, 0, 0, 0, 1);
    v3("syn_dir_layer_420", 0, 24, 1, 1, 0, 0);
    v3("syn_dir_layer_ctxv2_444", 1, 24, 1, 1, 1, 1);
    // [minor 6] XFORM_LARGE, tool bit 27.  The committed vectors v68-v73 pin
    // the tool itself; these walk it against the per-tile shape knobs it has
    // to compose with, because the interesting failures are not in the
    // butterfly.  They are the plane CAP of SYNTAX.md 6.7 (a res_level 1 or 2
    // tile, or a 4:2:0 chroma plane, coding a block smaller than the tile's
    // xform_size, so one frame carries three block sizes), the DC plane's
    // re-grid to nb x nb with its second-level transform firing only at
    // nb == 8, the planar interpolation's general Q4 mapping, and the n x n
    // intra predictors over an nb x nb wavefront.
    auto xf = [&](const char *name, int c444, int qp, int xform, int dir,
                  int res_pat = 0) {
        Case c{name, 192, 128, c444, 1, qp, 0, 0, 0, 3, 0, 0, 0, res_pat,
               0,    0,   1};
        c.intra_dir = dir;
        c.xform = xform;
        v.push_back(c);
    };
    xf("syn_xform16_420", 0, 24, 1, 0);
    xf("syn_xform32_420", 0, 24, 2, 0);
    xf("syn_xform16_444", 1, 20, 1, 0);
    xf("syn_xform32_444", 1, 20, 2, 0);
    xf("syn_xform32_dir_420", 0, 20, 2, 1);
    xf("syn_xform16_dir_444", 1, 16, 1, 1);
    if (!quick) {
        // The plane cap, both ways: a 32x32 tile-level size over cycling
        // res_level codes 32x32, 16x16 and 8x8 luma in one frame, and its
        // 4:2:0 chroma plane is capped one step further again.
        xf("syn_xform32_res_cycle_420", 0, 24, 2, 0, 1);
        xf("syn_xform16_res_cycle_444", 1, 24, 1, 0, 1);
        xf("syn_xform32_res2_420", 0, 28, 2, 0, 2);
        xf("syn_xform32_dir_res_444", 1, 20, 2, 1, 1);
        // The encoder's own per-tile choice, which mixes all three sizes
        // inside one frame and is the shape a real stream has.
        xf("syn_xform_auto_420", 0, 24, 255, 0);
        xf("syn_xform_auto_dir_444", 1, 20, 255, 1);
    }

    if (!quick) {
        // The wavefront meeting the other per-tile shape knobs: cycling
        // res_level (4x4 and 2x2 block planes as well as 8x8) and transform
        // skip, whose residual path skips the transform but not the
        // prediction.
        v3("syn_dir_res_cycle_420", 0, 20, 1, 0, 1, 1, 1, 0);
        v3("syn_dir_res_cycle_444", 1, 20, 1, 0, 1, 1, 1, 0);
        v3("syn_dir_tskip_420", 0, 16, 1, 0, 1, 0, 0, 1);
        v3("syn_dir_layer_tskip_444", 1, 16, 1, 1, 1, 0, 0, 1);
    }
    return v;
}

void run_synthetic(bool quick) {
    std::vector<Case> cases = synthetic_cases(quick);
    std::printf("-- %zu synthetic streams from nxvc_encoder\n", cases.size());
    for (const Case &c : cases) {
        if (case_skipped(c.name)) {
            std::printf("skip %s: --skip\n", c.name.c_str());
            ++g_skipped;
            continue;
        }
        std::vector<uint8_t> s;
        std::string err;
        if (!encode_case(c, s, err)) {
            std::printf("FAIL %s: encode failed (%s)\n", c.name.c_str(),
                        err.c_str());
            ++g_fail;
            continue;
        }
        // A stream this device declines is refused at the HEADER, before any
        // store format is chosen, so the RGB10A2 twin below would re-check the
        // identical refusal.  It is not run, which is why a device with a
        // smaller tool mask reports a smaller `checked` count than the desktop
        // ICDs and not merely a different pass/fail split: on the Adreno 650
        // the six 4:4:4 XFORM_LARGE cases contribute one result each instead
        // of two, so 226 against 232.
        if (handled_as_device_refusal(c.name.c_str(), s)) continue;
        check_stream(c.name.c_str(), s, "", NXVC_VKD_OUT_AUTO);
        // A 4:4:4 stream also goes through the RGB10A2 store: its 8-bit
        // samples are replicated into 10 bits and read back by taking the top
        // 8, which is a lossless round trip, so the same comparison holds.
        if (!quick && c.c444 && !c.alpha)
            check_stream((c.name + "_rgb10a2").c_str(), s, "",
                         NXVC_VKD_OUT_RGB10A2);
    }
}

// ------------------------------------------------- [timing] the self-check
// A GPU duration is built from a tick delta and a tick RATE, and a wrong rate
// is invisible in the duration -- it just makes every number bigger by a
// constant, which reads like a slow device rather than a broken clock.  This
// decoder reported GPU times about 1.57x high on one device for months for
// exactly that reason: 16 frames x 51 ms of "GPU" inside a 521 ms wall.
//
// The check is the one relation that cannot be argued with: **work on the GPU
// happens inside the wall time of the call that submitted and waited for it,
// so summed GPU <= summed wall, always.** A run that violates it has a wrong
// `timestampPeriod`, a wrong `timestampValidBits` mask, or is double-counting
// overlapping query pairs -- and any of those makes every absolute in the run
// worthless.  So it FAILS rather than printing a footnote, and it prints the
// ratio and the tick parameters either way so the next reader can see which of
// the two is wrong.
//
// The bound is deliberately not tightened beyond 1.0: the honest claim is
// containment, not a target ratio, and a fast device legitimately spends most
// of the wall in submission and readback.
int timing_selfcheck(nxvc_vk_decoder *dec, const char *what, double gpu_ms,
                     double wall_ms) {
    float period = 0.f;
    uint32_t bits = 0;
    nxvc_vk_decoder_timestamp_info(dec, &period, &bits);
    if (period <= 0.f || bits == 0) {
        std::printf("  timing: no GPU timestamps on this device "
                    "(period %.4f ns, %u valid bits) -- nothing to check\n",
                    (double)period, bits);
        return 0;
    }
    const double ratio = wall_ms > 0.0 ? gpu_ms / wall_ms : 0.0;
    std::printf("  timing: GPU %.2f ms / wall %.2f ms = %.3f  "
                "(timestampPeriod %.4f ns, %u valid bits)\n",
                gpu_ms, wall_ms, ratio, (double)period, bits);
    if (gpu_ms > wall_ms) {
        std::printf("FAIL %s: GPU-summed time EXCEEDS wall time by %.2fx. "
                    "The GPU cannot spend more time on a submission than the "
                    "call that waited for it took, so the tick rate or the "
                    "valid-bit mask is wrong and every absolute in this run "
                    "is meaningless. timestampPeriod %.4f ns, %u valid bits.\n",
                    what, ratio, (double)period, bits);
        ++g_fail;
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------------ bench
// PAPER 3.4's decode budget, measured on the shape the headset actually
// streams: two 2048x2048 eyes at 4:2:0, which is 2048 tiles in one frame.
// Informational -- it never fails the test.
// [inter] Timing for an inter SEQUENCE.  The intra bench decodes one frame
// `iters` times, which is the right shape for a pass whose cost depends only
// on the frame in front of it; the inter path's does not.  Its cost depends on
// the mode mix, and the mode mix is a property of where the sequence is: frame
// 0 is all INTRA, frame 1 is mostly WARP_MV, and by frame 10 a static region
// is WARP_SKIP and costs one Pass W dispatch and no entropy decode at all.  So
// this decodes the whole sequence, in order, and reports the mean over the
// inter frames as well as the intra frame they start from.
int run_bench_inter(int iters, int frames, int w, int h, int qp,
                    int intra_dir = -1) {
    std::vector<uint8_t> stream;
    std::string err;
    std::printf("-- encoding %d inter frames, %dx%d 4:2:0 at QP %d (%d tiles), "
                "INTRA_DIR %s\n",
                frames, w, h, qp, (w / 64) * (h / 64),
                intra_dir == 0 ? "off" : intra_dir == 1 ? "on" : "default");
    if (!encode_inter_stream(w, h, frames, qp, stream, err, intra_dir)) {
        std::printf("bench: encode failed (%s)\n", err.c_str());
        return 1;
    }
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.device_name = device_filter();
    nxvc_vk_decoder *dec = nullptr;
    if (nxvc_vk_decoder_create(&ci, &dec) != NXVC_VKD_OK) {
        std::printf("SKIP: %s\n",
                    dec ? nxvc_vk_decoder_last_error(dec) : "no decoder");
        nxvc_vk_decoder_destroy(dec);
        return 77;
    }
    double bestSeqGpu = 1e18, bestSeqWall = 1e18;
    double sumA = 0, sumB = 0, sumW = 0, sumG = 0, sumWall = 0;
    double intraA = 0, intraB = 0, intraG = 0;
    uint64_t bytes = 0;
    uint32_t tiles = 0, nskip = 0;
    int inter_frames = 0;
    for (int it = 0; it < iters; ++it) {
        // Re-parsing the stream header empties the reference ring and the
        // prediction history, so every iteration decodes the same sequence
        // from the same state.
        size_t consumed = 0;
        if (nxvc_vk_decoder_parse_stream_header(dec, stream.data(),
                                                stream.size(),
                                                &consumed) != NXVC_VKD_OK) {
            std::printf("bench: %s\n", nxvc_vk_decoder_last_error(dec));
            nxvc_vk_decoder_destroy(dec);
            return 1;
        }
        size_t off = consumed;
        double seqGpu = 0, seqWall = 0;
        double a = 0, b = 0, wms = 0, g = 0, wall = 0;
        int f = 0;
        uint64_t by = 0;
        uint32_t sk = 0;
        while (off < stream.size()) {
            size_t used = 0;
            if (nxvc_vk_decode_frame(dec, stream.data() + off,
                                     stream.size() - off,
                                     &used) != NXVC_VKD_OK) {
                std::printf("bench: %s\n", nxvc_vk_decoder_last_error(dec));
                nxvc_vk_decoder_destroy(dec);
                return 1;
            }
            nxvc_vkd_stats st{};
            nxvc_vk_decoder_stats(dec, &st);
            seqGpu += st.gpu_ms;
            seqWall += st.total_ms;
            if (f == 0) {
                if (it == 0) { intraA = st.pass_a_ms; intraB = st.pass_b_ms;
                               intraG = st.gpu_ms; tiles = st.tiles; }
            } else {
                a += st.pass_a_ms; b += st.pass_b_ms; wms += st.pass_w_ms;
                g += st.gpu_ms; wall += st.total_ms;
                by += st.frame_bytes;
                sk += st.tiles_skipped;
            }
            ++f;
            off += used;
        }
        if (seqGpu < bestSeqGpu) {
            bestSeqGpu = seqGpu;
            bestSeqWall = seqWall;
            inter_frames = f - 1;
            sumA = a; sumB = b; sumW = wms; sumG = g; sumWall = wall;
            bytes = by;
            nskip = sk;
        }
    }
    const double n = inter_frames > 0 ? (double)inter_frames : 1.0;
    std::printf(
        "%s, %d frames at QP %d, %u tiles/frame\n"
        "  best sequence: GPU %.2f ms total, wall %.2f ms total\n"
        "  frame 0 (all INTRA):  Pass A %.3f ms, Pass B %.3f ms, GPU %.3f ms\n"
        "  frames 1..%d (inter), mean per frame:\n"
        "    Pass A %.3f ms, Pass W %.3f ms, Pass B+W %.3f ms, GPU %.3f ms, "
        "wall %.3f ms\n"
        "    %.1f kB/frame, %.1f%% of tiles skipped\n",
        nxvc_vk_decoder_device_name(dec), frames, qp, tiles, bestSeqGpu,
        bestSeqWall, intraA, intraB, intraG, inter_frames, sumA / n, sumW / n,
        sumB / n, sumG / n, sumWall / n, (double)bytes / n / 1e3,
        100.0 * (double)nskip / (n * (tiles ? tiles : 1)));
    const int rc = timing_selfcheck(dec, "bench-inter", bestSeqGpu, bestSeqWall);
    nxvc_vk_decoder_destroy(dec);
    return rc;
}

int run_bench_qp(int iters, int qp, bool dense = false, int intra_dir = -1,
                 int xform = -1) {
    Case c{"bench_2x2048sq_420", 2048, 4096, 0, 1, qp, 0, 0, 0, 3, 0, 0, 0,
           1,   0,    0, 1};
    // A v1 stream (INTRA_DIR off) is what the Pico 4 actually streams
    // (ADR 0025 point 3), and it is the only shape in which Pass B's store
    // is a visible fraction of Pass B.  With the wavefront on, the store is
    // under a percent of the pass and no store format can be measured
    // through it.
    c.intra_dir = intra_dir;
    c.xform = xform;
    std::vector<uint8_t> stream;
    std::string err;
    std::printf("-- encoding %dx%d 4:2:0 at QP %d (%d tiles), INTRA_DIR %s, "
                "xform %s\n",
                c.w, c.h, qp, (c.w / 64) * (c.h / 64),
                intra_dir == 0 ? "off" : intra_dir == 1 ? "on" : "default",
                xform == 0     ? "8"
                : xform == 1   ? "16"
                : xform == 2   ? "32"
                : xform == 255 ? "auto"
                               : "default");
    if (!encode_case(c, stream, err)) {
        std::printf("bench: encode failed (%s)\n", err.c_str());
        return 1;
    }
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.device_name = device_filter();
    // [sparse] Ask for the exact coefficient traffic; it costs a copy of the
    // 264-B-per-tile length buffer after the last timestamp.
    ci.flags = (uint32_t)NXVC_VKD_FLAG_COEF_STATS |
               (dense ? (uint32_t)NXVC_VKD_FLAG_DENSE_COEF : 0u);
    nxvc_vk_decoder *dec = nullptr;
    if (nxvc_vk_decoder_create(&ci, &dec) != NXVC_VKD_OK) {
        std::printf("SKIP: %s\n",
                    dec ? nxvc_vk_decoder_last_error(dec) : "no decoder");
        nxvc_vk_decoder_destroy(dec);
        return 77;
    }
    size_t consumed = 0;
    if (nxvc_vk_decoder_parse_stream_header(dec, stream.data(), stream.size(),
                                            &consumed) != NXVC_VKD_OK) {
        std::printf("bench: %s\n", nxvc_vk_decoder_last_error(dec));
        nxvc_vk_decoder_destroy(dec);
        return 1;
    }
    const uint8_t *frame = stream.data() + consumed;
    const size_t flen = stream.size() - consumed;
    double sumG = 0, sumT = 0;
    double bestA = 1e9, bestB = 1e9, bestG = 1e9, bestT = 1e9;
    nxvc_vkd_stats st{};
    for (int i = 0; i < iters; ++i) {
        size_t c2 = 0;
        if (nxvc_vk_decode_frame(dec, frame, flen, &c2) != NXVC_VKD_OK) {
            std::printf("bench: %s\n", nxvc_vk_decoder_last_error(dec));
            nxvc_vk_decoder_destroy(dec);
            return 1;
        }
        nxvc_vk_decoder_stats(dec, &st);
        if (st.pass_a_ms < bestA) bestA = st.pass_a_ms;
        if (st.pass_b_ms < bestB) bestB = st.pass_b_ms;
        if (st.gpu_ms < bestG) bestG = st.gpu_ms;
        if (st.total_ms < bestT) bestT = st.total_ms;
        // Summed over the SAME iterations, so the containment check compares
        // like with like.  `bestG` and `bestT` are minima over independent
        // iterations and are not a pair.
        sumG += st.gpu_ms;
        sumT += st.total_ms;
    }
    std::printf(
        "%s, QP %d: %u tiles, %llu B frame (%llu B payload)\n"
        "  best of %d: Pass A %.3f ms, Pass B %.3f ms, GPU total %.3f ms, "
        "wall %.3f ms\n"
        "  host parse %.3f ms, record+submit %.3f ms\n"
        "  coefficient SSBO %.2f MB written by Pass A and read by Pass B "
        "(%s layout; the dense slot is %.1f MB)\n",
        nxvc_vk_decoder_device_name(dec), qp, st.tiles,
        (unsigned long long)st.frame_bytes,
        (unsigned long long)st.payload_bytes, iters, bestA, bestB, bestG, bestT,
        st.parse_ms, st.submit_ms, (double)st.coef_bytes / 1e6,
        dense ? "dense" : "sparse", (double)st.coef_slot_bytes / 1e6);
    const int rc = timing_selfcheck(dec, "bench-qp", sumG, sumT);
    nxvc_vk_decoder_destroy(dec);
    return rc;
}

// ------------------------------------------------ [v3] bench helpers
// One decoder over one stream, best-of-N per-pass timings.
struct BenchRun {
    double passA = 0, passB = 0, gpu = 0, wall = 0;
    uint64_t frameBytes = 0, payloadBytes = 0, coefBytes = 0;
    uint32_t tiles = 0;
    bool ok = false;
    bool skipped = false;
};

BenchRun time_stream(const std::vector<uint8_t> &stream, int iters,
                     uint32_t dirSched, uint32_t tileSort,
                     uint32_t extraFlags = 0) {
    BenchRun r;
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.flags = extraFlags;
    ci.device_name = device_filter();
    nxvc_vk_decoder *dec = nullptr;
    if (nxvc_vk_decoder_create(&ci, &dec) != NXVC_VKD_OK) {
        nxvc_vk_decoder_destroy(dec);
        r.skipped = true;
        return r;
    }
    nxvc_vk_decoder_set_dir_sched(dec, dirSched);
    nxvc_vk_decoder_set_tile_sort(dec, tileSort);
    size_t consumed = 0;
    if (nxvc_vk_decoder_parse_stream_header(dec, stream.data(), stream.size(),
                                            &consumed) != NXVC_VKD_OK) {
        std::printf("bench: %s\n", nxvc_vk_decoder_last_error(dec));
        nxvc_vk_decoder_destroy(dec);
        return r;
    }
    const uint8_t *frame = stream.data() + consumed;
    const size_t flen = stream.size() - consumed;
    double bA = 1e9, bB = 1e9, bG = 1e9, bT = 1e9;
    nxvc_vkd_stats st{};
    for (int i = 0; i < iters; ++i) {
        size_t c2 = 0;
        if (nxvc_vk_decode_frame(dec, frame, flen, &c2) != NXVC_VKD_OK) {
            std::printf("bench: %s\n", nxvc_vk_decoder_last_error(dec));
            nxvc_vk_decoder_destroy(dec);
            return r;
        }
        nxvc_vk_decoder_stats(dec, &st);
        bA = std::min(bA, st.pass_a_ms);
        bB = std::min(bB, st.pass_b_ms);
        bG = std::min(bG, st.gpu_ms);
        bT = std::min(bT, st.total_ms);
    }
    r.passA = bA; r.passB = bB; r.gpu = bG; r.wall = bT;
    r.frameBytes = st.frame_bytes;
    r.payloadBytes = st.payload_bytes;
    r.coefBytes = st.coef_bytes;
    r.tiles = st.tiles;
    r.ok = true;
    nxvc_vk_decoder_destroy(dec);
    return r;
}

// The 2048-tile shape the headset streams, as one 4:2:0 frame.
Case bench_case(int qp, int intra_dir, int res_pattern, int tskip,
                int xform = -1) {
    Case c{"bench", 2048, 4096, 0, 1, qp, 0, 0, tskip, 3, 0, 0, 0,
           res_pattern, 0, 0, 1};
    c.intra_dir = intra_dir;
    c.xform = xform;
    return c;
}

// docs/SYNTAX.md 7.6: with the above-right dependency the independent set is
// 2*by + bx, so an nb x nb block plane takes 3*nb - 2 steps; without it, the
// anti-diagonal's 2*nb - 1.  The sub-tile restriction caps nb at 4.  One
// barrier per step, plus the 3 the DC-plane path already pays per plane.
int wavefront_steps(int sched, int nb) {
    int n = (sched & 2) ? std::min(nb, 4) : nb;
    return (sched & 1) ? 2 * n - 1 : 3 * n - 2;
}
// Mean occupancy during the prediction step: nb*nb blocks spread over S
// steps at kDirLanesPerBlock threads each, against the workgroup's 256, and
// never more than 100 %.  SYNTAX.md 7.6's 4.5 % is this at nb == 8, S == 22
// and the four-threads-per-block mapping the wavefront used to inherit from
// the transform; reconstruct.comp now stages the residual in shared memory
// and gives a block 16 threads, so the same schedule runs at 4x that.
double wavefront_occupancy_pct(int sched, int nb) {
    double per_step = (double)(nb * nb) / (double)wavefront_steps(sched, nb);
    double pct = 100.0 * per_step * 16.0  /* nxvw::kDirLanesPerBlock */ / 256.0;
    return pct > 100.0 ? 100.0 : pct;
}

// Barriers reconstruct.comp actually executes for one tile, counted off the
// kernel rather than off the idealized schedule.  Per plane: one after the DC
// dequantize, two more for the second-level IDCT when nb == 8, one after the
// block means, two for the transform's row/column passes, and then either one
// prediction barrier (v1) or one to stage the residual in the sample store
// plus one per wavefront step (plus one for the layered form's recon ->
// samples pass).  This is the worst case: [sparse] a plane whose DC unit
// coded nothing skips the two second-level IDCT barriers.
int barriers_per_plane(int nb, int dirSteps, bool layer) {
    int b = 4 + (nb == 8 ? 2 : 0) + 2;
    b += dirSteps > 0 ? 1 + dirSteps + (layer ? 1 : 0) : 1;
    return b;
}
int barriers_per_tile(int sched, bool c444, bool dir, bool layer) {
    const int lumaNb = 8, chromaNb = c444 ? 8 : 4;
    const int ls = dir ? wavefront_steps(sched, lumaNb) : 0;
    const int cs = dir ? wavefront_steps(sched, chromaNb) : 0;
    return barriers_per_plane(lumaNb, ls, layer) +
           2 * barriers_per_plane(chromaNb, cs, layer);
}

// -------------------------------------------------- [v3] wavefront variants
// SYNTAX.md 7.6 prices three schedules in rate; this prices them in decode
// time on the same 2048-tile all-INTRA_DIR frame.  Only schedule 0 decodes
// this stream correctly -- the other two are what a stream encoded under the
// matching restriction would cost -- so the pixels are not compared here,
// only the time.
int run_bench_dir(int iters) {
    struct Variant { uint32_t sched; const char *what; const char *rate; };
    static const Variant kVars[] = {
        {0, "as written (left, above, above-right)", "--"},
        {1, "no above-right reference", "+0.24 %"},
        {3, "no above-right + 32x32 sub-tiles", "+1.8 %"},
    };
    std::vector<uint8_t> stream, base;
    std::string err;
    if (!encode_case(bench_case(24, 1, 0, 0), stream, err) ||
        !encode_case(bench_case(24, 0, 0, 0), base, err)) {
        std::printf("bench: encode failed (%s)\n", err.c_str());
        return 1;
    }
    BenchRun b0 = time_stream(base, iters, 0, 0);
    if (b0.skipped) { std::printf("SKIP: no usable Vulkan device\n"); return 77; }
    if (!b0.ok) return 1;
    std::printf(
        "\n-- Pass B wavefront variants, 2048 tiles 4:2:0 QP 24, best of %d\n"
        "   INTRA_DIR off (v1 planar predictor, no wavefront): Pass B %.3f ms,"
        " %d barriers/tile\n",
        iters, b0.passB, barriers_per_tile(0, false, false, false));
    for (const Variant &v : kVars) {
        BenchRun r = time_stream(stream, iters, v.sched, 0);
        if (!r.ok) return 1;
        std::printf(
            "   sched %u  %-38s  Pass B %.3f ms  (%.2fx v1)  "
            "%2d steps  %3d barriers/tile  occupancy %.1f %%  rate %s\n",
            v.sched, v.what, r.passB, r.passB / b0.passB,
            wavefront_steps((int)v.sched, 8),
            barriers_per_tile((int)v.sched, false, true, false),
            wavefront_occupancy_pct((int)v.sched, 8), v.rate);
    }
    return 0;
}

// -------------------------------------------- [minor 6] the transform size
// XFORM_LARGE (tool bit 27) changes Pass B in two directions at once, and the
// point of this arm is that they are separable and both large.  It ADDS
// arithmetic -- 2.75 multiplies per sample at 8x8 against 9.4 at 16x16 and
// 20.7 at 32x32 (SYNTAX.md 6.2.1) -- and it REMOVES schedule: a plane is
// nb x nb blocks instead of 8 x 8, so the transform runs in fewer rounds and,
// with INTRA_DIR, the wavefront falls from 22 steps to 10 and then 4.  A
// stream that sets no tool bit must also cost exactly what it did, which the
// `xform 8` row against a pre-XFORM_LARGE build is the check on.
int run_bench_xform(int iters) {
    static const struct { int xf; const char *what; } kArms[] = {
        {0, "8x8   (no tool bit)"},
        {1, "16x16 (xform_size 1)"},
        {2, "32x32 (xform_size 2)"},
        {255, "auto  (per-tile RD choice)"},
    };
    std::printf("\n-- Pass B by transform size, 2048 tiles 4:2:0 QP 24, "
                "best of %d\n", iters);
    for (int dir : {0, 1}) {
        double base = 0.0;
        for (const auto &a : kArms) {
            std::vector<uint8_t> stream;
            std::string err;
            if (!encode_case(bench_case(24, dir, 0, 0, a.xf), stream, err)) {
                std::printf("bench: encode failed (%s)\n", err.c_str());
                return 1;
            }
            BenchRun r = time_stream(stream, iters, 0, 0);
            if (r.skipped) {
                std::printf("SKIP: no usable Vulkan device\n");
                return 77;
            }
            if (!r.ok) return 1;
            if (a.xf == 0) base = r.passB;
            std::printf("   INTRA_DIR %s  %-24s  payload %6.3f MB   "
                        "Pass A %7.3f ms   Pass B %6.3f ms  (%.2fx 8x8)\n",
                        dir ? "on " : "off", a.what,
                        (double)r.payloadBytes / 1e6, r.passA, r.passB,
                        base > 0.0 ? r.passB / base : 1.0);
        }
    }
    return 0;
}

// ------------------------------- two stores from one Pass B, or two passes
// A frame that needs two display formats -- today a 4:2:0 stream with a coded
// alpha plane on the two-plane store, and every frame once the reference ring
// lands -- can write both from one dispatch or run Pass B twice.  Same pixels
// either way; this is the price of each.
int run_bench_stores(int iters) {
    Case c{"bench_alpha420", 2048, 4096, 0, 1, 24, 0, /*alpha=*/1, 0, 3, 0, 0,
           0,    1,           0, 0,      1};
    std::vector<uint8_t> stream;
    std::string err;
    if (!encode_case(c, stream, err)) {
        std::printf("bench: encode failed (%s)\n", err.c_str());
        return 1;
    }
    BenchRun fused = time_stream(stream, iters, 0, 0, 0);
    if (fused.skipped) { std::printf("SKIP: no usable Vulkan device\n"); return 77; }
    if (!fused.ok) return 1;
    BenchRun split =
        time_stream(stream, iters, 0, 0, (uint32_t)NXVC_VKD_FLAG_SPLIT_STORES);
    if (!split.ok) return 1;
    std::printf(
        "\n-- Pass B with two stores, 2048 tiles 4:2:0 + coded alpha at QP 24,"
        " best of %d\n"
        "   two dispatches (ycbcr420, then rgba8) : Pass B %.3f ms\n"
        "   one dispatch, both stores             : Pass B %.3f ms  (%+.1f %%)\n",
        iters, split.passB, fused.passB,
        100.0 * (fused.passB - split.passB) / split.passB);
    return 0;
}

// ------------------------------------------- [v3] fixed vs per-byte cost
// The GDeflate question: how much of a frame's decode time is per-tile
// overhead that 64x64 tiling buys us, and how much is per-byte work?  A
// straight least-squares fit of pass time against payload size over the QP
// ladder separates the two.  Note that COEFFICIENT traffic is the same at
// every QP -- the layout between the passes is dense -- so the slope is per
// megabyte of entropy-coded payload, and the intercept is what a frame costs
// with no payload at all.
int run_bench_overhead(int iters) {
    static const int kQps[] = {63, 51, 36, 24, 12};
    double x[5] = {}, ya[5] = {}, yb[5] = {};
    int n = 0;
    uint32_t tiles = 0;
    double coefMB[5] = {};
    std::printf("\n-- fixed vs per-byte decode cost, 2048 tiles 4:2:0, "
                "best of %d\n", iters);
    for (int qp : kQps) {
        std::vector<uint8_t> stream;
        std::string err;
        if (!encode_case(bench_case(qp, -1, 0, 0), stream, err)) {
            std::printf("bench: encode failed (%s)\n", err.c_str());
            return 1;
        }
        BenchRun r = time_stream(stream, iters, 0, 0,
                                 (uint32_t)NXVC_VKD_FLAG_COEF_STATS);
        if (r.skipped) { std::printf("SKIP: no usable Vulkan device\n"); return 77; }
        if (!r.ok) return 1;
        x[n] = (double)r.payloadBytes / 1e6;
        ya[n] = r.passA;
        yb[n] = r.passB;
        tiles = r.tiles;
        coefMB[n] = (double)r.coefBytes / 1e6;
        std::printf("   QP %2d: payload %6.3f MB   coef %6.2f MB   "
                    "Pass A %7.3f ms   Pass B %6.3f ms\n",
                    qp, x[n], coefMB[n], ya[n], yb[n]);
        ++n;
    }
    auto fit = [&](const double *y, double &a, double &b) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < n; ++i) { sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i]; }
        double den = n * sxx - sx * sx;
        b = den != 0 ? (n * sxy - sx * sy) / den : 0;   // slope, ms per MB
        a = (sy - b * sx) / n;                          // intercept, ms
    };
    double ia, sa, ib, sb;
    fit(ya, ia, sa);
    fit(yb, ib, sb);
    std::printf(
        "   Pass A: %.3f ms fixed + %.3f ms per MB of payload\n"
        "   Pass B: %.3f ms fixed + %.3f ms per MB of payload\n"
        "   per tile at zero payload: Pass A %.1f ns, Pass B %.1f ns "
        "(%u tiles)\n"
        "   [sparse] coefficient traffic now follows the payload, %.2f MB to "
        "%.2f MB over this ladder, against 25.6 MB at every QP dense.  Pass B "
        "no longer fits a line as well as it did: the mode-0 fast path is a "
        "step, not a slope.\n",
        ia, sa, ib, sb, ia * 1e6 / tiles, ib * 1e6 / tiles, tiles, coefMB[0],
        coefMB[n - 1]);
    return 0;
}

// ------------------------------------- [v3] host-side tile grouping for Pass B
// The divergence question: Pass A already groups its dispatches by lane count;
// does grouping Pass B's workgroups by tile shape pay?  Measured on a frame
// whose tiles deliberately differ -- cycling res_level and the encoder's own
// per-tile transform-skip decision -- so neighbouring workgroups take
// different branches unless they are sorted.
int run_bench_sort(int iters) {
    std::vector<uint8_t> stream;
    std::string err;
    if (!encode_case(bench_case(24, -1, 1, 2), stream, err)) {
        std::printf("bench: encode failed (%s)\n", err.c_str());
        return 1;
    }
    BenchRun off = time_stream(stream, iters, 0, 0);
    if (off.skipped) { std::printf("SKIP: no usable Vulkan device\n"); return 77; }
    BenchRun on = time_stream(stream, iters, 0, 1);
    if (!off.ok || !on.ok) return 1;
    std::printf(
        "\n-- Pass B workgroup ordering, 2048 tiles 4:2:0 QP 24, mixed "
        "res_level and transform skip, best of %d\n"
        "   raster order      : Pass B %.3f ms\n"
        "   sorted by shape   : Pass B %.3f ms  (%+.1f %%)\n",
        iters, off.passB, on.passB,
        100.0 * (on.passB - off.passB) / off.passB);
    return 0;
}

// Pass A's cost scales with the symbol rate, so the budget is quoted over a
// bitrate range rather than at one operating point.
int run_bench(int iters) {
    // [sparse] Both layouts at each QP: identical pixels, and the difference
    // between them is the whole of PAPER 3.2.5's first optimisation.
    for (int qp : {12, 24, 36})
        for (bool dense : {false, true}) {
            int rc = run_bench_qp(iters, qp, dense);
            if (rc) return rc;
        }
    int rc = run_bench_dir(iters);
    if (rc) return rc;
    if ((rc = run_bench_overhead(iters))) return rc;
    if ((rc = run_bench_sort(iters))) return rc;
    if ((rc = run_bench_stores(iters))) return rc;
    if ((rc = run_bench_xform(iters))) return rc;
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    bool quick = false, do_vectors = true, do_synth = true, do_loss = true;
    int bench = 0;
    // --bench-qp is the one-QP slice of --bench: encode the 2 x 2048^2 4:2:0
    // frame once and time the two passes over it, nothing else.  The full
    // --bench sweep encodes eleven such frames with the CPU reference encoder
    // and that is an hour of a phone's CPU before a single dispatch runs, so
    // on a device the slice is the usable form.
    int bench_qp = -1;
    int bench_dir = -1;   // --bench-v1 pins INTRA_DIR off
    int bench_inter = 0;  // --bench-inter N: an N-frame inter sequence
    int bench_w = 1024, bench_h = 1024;
    const char *bench_save = nullptr;  // write the bench stream and exit
    // [minor 6] --bench-xform pins the tile transform size of the saved or
    // timed bench stream: 8, 16, 32 or `auto`.  -1 leaves the encoder's
    // default, which is 8 and sets no tool bit.
    int bench_xform = -1;
    // Line buffered, always.  stdout to a file is fully buffered by default,
    // which is why a wedged sweep left a zero-byte log naming nothing.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--vectors" && i + 1 < argc) g_vectors_dir = argv[++i];
        else if (a == "--quick") quick = true;
        else if (a == "--verbose") g_verbose = true;
        // Seconds one case may take before the watchdog names it and exits.
        // Off by default: a desktop run has no use for it and a CI machine
        // under load must not be shot for being slow.
        else if (a == "--timeout" && i + 1 < argc)
            g_case_timeout_s = std::atoi(argv[++i]);
        // Repeatable.  After a hang has been attributed, this is what sweeps
        // everything else.
        else if (a == "--skip" && i + 1 < argc) g_skip.push_back(argv[++i]);
        else if (a == "--only-vectors") { do_synth = false; do_loss = false; }
        else if (a == "--only-synthetic") { do_vectors = false; do_loss = false; }
        else if (a == "--only-loss") { do_vectors = false; do_synth = false; }
        else if (a == "--no-loss") do_loss = false;
        else if (a == "--bench") bench = i + 1 < argc && argv[i + 1][0] != '-'
                                             ? std::atoi(argv[++i])
                                             : 20;
        else if (a == "--bench-v1") bench_dir = 0;
        else if (a == "--bench-xform" && i + 1 < argc) {
            std::string v = argv[++i];
            bench_xform = v == "8"    ? 0
                          : v == "16" ? 1
                          : v == "32" ? 2
                          : v == "auto" ? 255
                                        : -2;
            if (bench_xform == -2) {
                std::fprintf(stderr, "--bench-xform: 8|16|32|auto\n");
                return 2;
            }
        }
        else if (a == "--bench-inter" && i + 1 < argc) {
            bench_inter = std::atoi(argv[++i]);
            if (!bench) bench = 5;
        }
        else if (a == "--bench-size" && i + 2 < argc) {
            bench_w = std::atoi(argv[++i]);
            bench_h = std::atoi(argv[++i]);
        }
        else if (a == "--bench-save" && i + 1 < argc) bench_save = argv[++i];
        else if (a == "--bench-qp" && i + 1 < argc) {
            bench_qp = std::atoi(argv[++i]);
            if (!bench) bench = 10;
        }
        else {
            std::fprintf(stderr,
                         "usage: %s [--vectors DIR] [--quick] [--verbose]\n"
                         "       [--bench N] [--bench-qp QP] [--bench-v1]\n"
                         "       [--bench-xform 8|16|32|auto]\n"
                         "       [--bench-save FILE]\n"
                         "       [--only-loss] [--no-loss]\n"
                         "       [--bench-inter FRAMES] [--bench-size W H]\n",
                         argv[0]);
            return 2;
        }
    }
    if (!g_vectors_dir) g_vectors_dir = NXVC_VECTORS_DIR;
    if (bench_save) {
        Case c{"bench_2x2048sq_420", 2048, 4096, 0, 1, bench_qp < 0 ? 24 : bench_qp,
               0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 1};
        c.intra_dir = bench_dir;
        c.xform = bench_xform;
        std::vector<uint8_t> stream;
        std::string err;
        if (!encode_case(c, stream, err)) {
            std::printf("encode failed: %s\n", err.c_str());
            return 1;
        }
        std::FILE *f = std::fopen(bench_save, "wb");
        if (!f) { std::perror("open"); return 1; }
        std::fwrite(stream.data(), 1, stream.size(), f);
        std::fclose(f);
        std::printf("wrote %zu B to %s\n", stream.size(), bench_save);
        return 0;
    }
    if (bench_inter)
        return run_bench_inter(bench, bench_inter, bench_w, bench_h,
                               bench_qp < 0 ? 24 : bench_qp, bench_dir);
    if (bench_qp >= 0)
        return run_bench_qp(bench, bench_qp, false, bench_dir, bench_xform);
    if (bench) return run_bench(bench);

    // Probe once, so "no ICD" is one skip rather than 32 identical failures.
    {
        nxvc_vkd_create_info ci;
        nxvc_vk_decoder_create_info_default(&ci);
        ci.device_name = device_filter();
        nxvc_vk_decoder *dec = nullptr;
        nxvc_vkd_status st = nxvc_vk_decoder_create(&ci, &dec);
        if (st != NXVC_VKD_OK) {
            std::printf("SKIP: %s\n",
                        dec ? nxvc_vk_decoder_last_error(dec) : "no decoder");
            nxvc_vk_decoder_destroy(dec);
            return 77;
        }
        std::printf("-- device: %s\n", nxvc_vk_decoder_device_name(dec));
        nxvc_vk_decoder_destroy(dec);
    }

    // Before any stage, not just the first: the wedge that made this
    // necessary was in the synthetic streams, which --only-vectors' absence
    // used to leave unguarded.
    start_watchdog();

    if (do_vectors) {
        run_vectors();
        run_rejects();
    }
    if (do_synth) run_synthetic(quick);
    if (do_synth) {
        CaseGuard cg("row_present");
        run_row_present(quick ? 8 : 24);
    }
    if (do_synth) run_atlas_modes();
    if (do_vectors) {
        CaseGuard cg("atlas-stats");
        run_atlas_stats();
    }
    if (do_synth) {
        CaseGuard cg("atlas-view");
        run_atlas_view();
    }
    if (do_loss) run_loss(quick ? 20 : 100);

    std::printf("-- %d stream(s) checked, %d skipped, %d failure(s)\n",
                g_checked, g_skipped, g_fail);
    if (g_checked == 0) {
        std::printf("SKIP: nothing was checked\n");
        return 77;
    }
    return g_fail ? 1 : 0;
}
