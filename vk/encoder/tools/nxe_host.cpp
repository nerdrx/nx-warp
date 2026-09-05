/* nxe_host.cpp -- see nxe_host.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nxe_host.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstring>

extern "C" {
#include "nxe_tables.h"
}

/* The built-in probability tables come from the reference library: they are a
 * trained data set, not an algorithm, and a second copy of them in this tree
 * would be a second thing to keep in step.  `ref/src/tables.cpp` is compiled
 * into this tool for exactly this reason, and `ref/src` is on this target's
 * include path (see the CMakeLists, which only defines the target when
 * `ref/src/tables.cpp` exists).
 *
 * This used to be a hand-written mirror of `nxvc::CtxTable` and
 * `nxvc::TableSet` so that the declaration was local.  It must not be: at
 * bitstream minor 6 `kNumCtx` went from 16 to 27, `build_default_set` writes
 * all `kNumCtx` rows, and the mirror was still 16 rows -- so every call wrote
 * 11990 bytes past the end of a stack object.  A layout this tool does not own
 * is not a thing to transcribe; include the header the definition lives in. */
#include "common.h"

namespace nxe {

/* ------------------------------------------------------------------ setup */
void setup(const Config &cfg, Frame &f) {
    nxe_frame_params &fp = f.fp;
    std::memset(&fp, 0, sizeof fp);
    fp.width = (uint32_t)(cfg.w / cfg.eyes);
    fp.height = (uint32_t)cfg.h;
    fp.eyes = (uint32_t)cfg.eyes;
    fp.tiles_x = (fp.width + 63) / 64;
    fp.tiles_y = (fp.height + 63) / 64;
    fp.ntiles = fp.eyes * fp.tiles_x * fp.tiles_y;
    fp.chroma420 = cfg.chroma444 ? 0u : 1u;
    fp.base_qp = (uint32_t)cfg.qp;
    fp.chroma_qp_off = cfg.chroma_qp_off;
    fp.nctx = cfg.ctx_v3 ? NXE_NCTX_V3
                         : (cfg.ctx_v2 ? NXE_NCTX_V2 : NXE_NCTX_V1);
    fp.sdh = cfg.sign_hide ? 1u : 0u;
    fp.intra_dir = cfg.intra_dir ? 1u : 0u;
    fp.dir_layer = cfg.dir_layer ? 1u : 0u;
    fp.nsub_log2 = (uint32_t)cfg.nsub_log2;
    fp.quant_matrix = (uint32_t)cfg.matrix;
    fp.tables_present = 0;                 /* static per-frame tables */
    fp.frame_flags = 1;                    /* bit 0: tile-map reset */
    if (cfg.intra_dir && cfg.dir_layer) fp.frame_flags |= 4;
    fp.ycocgr = 0;
    for (int i = 0; i < 64; ++i) {
        int m = cfg.matrix < 0 ? 0 : (cfg.matrix > 3 ? 3 : cfg.matrix);
        fp.wm_luma[i] = nxe_weight[m][i];
        fp.wm_chroma[i] = nxe_weight[m == 0 ? 0 : 3][i];
    }

    f.jobs.assign(fp.ntiles, nxe_tile_job{});
    for (uint32_t row = 0; row < fp.tiles_y; ++row)
        for (uint32_t eye = 0; eye < fp.eyes; ++eye)
            for (uint32_t col = 0; col < fp.tiles_x; ++col) {
                uint32_t t = row * fp.eyes * fp.tiles_x + eye * fp.tiles_x + col;
                nxe_tile_job &j = f.jobs[t];
                j.tile = t;
                j.col = col;
                j.row = row;
                j.eye = eye;
                j.qp_delta = 0;
                /* ref's make_tile_params seed; choose_table_sets replaces it. */
                j.table_set = (uint32_t)((cfg.qp >> 3) < 0 ? 0
                                         : ((cfg.qp >> 3) > 7 ? 7 : (cfg.qp >> 3)));
                j.tskip = (uint32_t)cfg.tskip;
                j.wm_id = (uint32_t)cfg.wm_id;
                j.chroma444 = cfg.chroma444 ? 1u : 0u;
                j.res_level = 0;
                j.mode = 3;                /* NXVC_MODE_INTRA */
                j.nsub_log2 = (uint32_t)cfg.nsub_log2;
            }

    int base = 0;
    for (int p = 0; p < NXE_MAX_PLANES; ++p) {
        f.plane_size[p] = nxe_plane_size(&fp, &f.jobs[0], p);
        f.plane_words[p] = f.plane_size[p] * f.plane_size[p] / 2;
        f.plane_base[p] = base;
        base += (int)fp.ntiles * f.plane_words[p];
        f.src[p].assign((size_t)fp.ntiles * f.plane_size[p] * f.plane_size[p], 0);
    }
    f.src_packed.assign((size_t)base * 2, 0);   /* u16 halves of `base` words */
    f.coef.assign((size_t)fp.ntiles * NXE_TILE_COEFS_MAX, 0);
    f.modes.assign((size_t)fp.ntiles * 3 * 64, 0);
    f.slots.assign((size_t)fp.ntiles * NXE_TILE_BYTES_MAX, 0);
    f.tile_bytes.assign(fp.ntiles, 0);
    f.tile_prefix.assign(fp.ntiles, 0);
}

/* The storage bound here has to cover every row `build_default_set` writes,
 * and the reference's context count is the thing that moves (12 -> 16 -> 27).
 * Assert it rather than tracking it by hand: growing `kNumCtx` past
 * NXE_MAX_CTX must be a compile error in this file, not a truncated table. */
static_assert(nxvc::kNumCtx <= NXE_MAX_CTX,
              "NXE_MAX_CTX is smaller than the reference's context count");
static_assert(nxvc::kNumSym == NXE_NUM_SYM, "symbol count disagrees with ref");

void build_tables(const Config &cfg, Frame &f) {
    std::memset(&f.tabs, 0, sizeof f.tabs);
    const int nctx = nxvc::coded_context_count(cfg.ctx_v2, cfg.ctx_v3);
    for (int k = 0; k < 8; ++k) {
        nxvc::TableSet ts;
        nxvc::build_default_set(ts, k, nctx);
        for (int c = 0; c < nxvc::kNumCtx; ++c)
            for (int s = 0; s < NXE_NUM_SYM; ++s) {
                f.tabs.freq[k][c][s] = ts.ctx[c].freq[s];
                f.tabs.cum[k][c][s] = ts.ctx[c].cum[s];
            }
    }
    /* The cost table choose_table_sets() reads.  A zero frequency gives
     * -log2(0) = +inf and a candidate that codes an impossible symbol is
     * therefore infinitely expensive, which is exactly what the expression it
     * replaces produced. */
    f.log_freq.assign((size_t)8 * nxvc::kNumCtx * NXE_NUM_SYM, 0.0);
    for (int k = 0; k < 8; ++k)
        for (int c = 0; c < nxvc::kNumCtx; ++c)
            for (int s = 0; s < NXE_NUM_SYM; ++s)
                f.log_freq[((size_t)k * nxvc::kNumCtx + c) * NXE_NUM_SYM + s] =
                    std::log2((double)f.tabs.freq[k][c][s] / 1024.0);
}

/* ------------------------------------------------------------------- input
 *
 * This stands in for E0.  The real E0 imports a compositor VkImage; a file of
 * planar YUV has no import format, so the repack is done here in exactly the
 * terms `load_tile` uses at res_level 0 and factor 1: a clamped fetch inside
 * the eye's own sub-picture, so a left-eye tile can never read a right-eye
 * sample and a frame that is not a multiple of 64 is edge-replicated.
 */
static int fetch_clamped(const uint8_t *p, int stride, int w, int h, int x, int y) {
    x = x < 0 ? 0 : (x >= w ? w - 1 : x);
    y = y < 0 ? 0 : (y >= h ? h - 1 : y);
    return p[(size_t)y * stride + x];
}

/* The repack itself, shared by read_frame() and load_planes() so the file
 * path and the library path can never lay a picture out differently. */
static void repack_planes(const Config &cfg, Frame &f,
                          const uint8_t *const pl[3], const int stride[3]) {
    const nxe_frame_params &fp = f.fp;
    /* Per-eye plane extent, ref's Geometry::pw / ::ph. */
    const int pw[3] = {(int)fp.width, cfg.chroma444 ? (int)fp.width
                                                    : ((int)fp.width + 1) / 2,
                       cfg.chroma444 ? (int)fp.width : ((int)fp.width + 1) / 2};
    const int ph[3] = {(int)fp.height, cfg.chroma444 ? (int)fp.height
                                                     : ((int)fp.height + 1) / 2,
                       cfg.chroma444 ? (int)fp.height : ((int)fp.height + 1) / 2};

    for (uint32_t t = 0; t < fp.ntiles; ++t) {
        const nxe_tile_job &j = f.jobs[t];
        for (int p = 0; p < NXE_MAX_PLANES; ++p) {
            const int size = f.plane_size[p];
            const int sub = (p && fp.chroma420) ? 2 : 1;
            const int ox = (int)j.col * (64 / sub), oy = (int)j.row * (64 / sub);
            const uint8_t *base = pl[p] + (size_t)j.eye * pw[p];
            int32_t *dst = &f.src[p][(size_t)t * size * size];
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    dst[(size_t)y * size + x] = fetch_clamped(
                        base, stride[p], pw[p], ph[p], ox + x, oy + y);
            /* The packed int16 form the GPU reads, two samples per word. */
            uint16_t *pk = &f.src_packed[(size_t)(f.plane_base[p] +
                                                  (int)t * f.plane_words[p]) * 2];
            for (int i = 0; i < size * size; ++i)
                pk[i] = (uint16_t)(int16_t)dst[i];
        }
    }
}

void load_planes(const Config &cfg, Frame &f,
                 const uint8_t *y, size_t y_stride,
                 const uint8_t *cb, const uint8_t *cr, size_t chroma_stride) {
    const uint8_t *pl[3] = {y, cb, cr};
    const int stride[3] = {(int)y_stride, (int)chroma_stride, (int)chroma_stride};
    repack_planes(cfg, f, pl, stride);
}

bool read_frame(std::FILE *fi, const Config &cfg, Frame &f) {
    const int W = cfg.w, H = cfg.h;
    const int cw = cfg.chroma444 ? W : (W + 1) / 2;
    const int ch = cfg.chroma444 ? H : (H + 1) / 2;
    static std::vector<uint8_t> Y, U, V;
    Y.resize((size_t)W * H);
    U.resize((size_t)cw * ch);
    V.resize((size_t)cw * ch);
    if (std::fread(Y.data(), 1, Y.size(), fi) != Y.size()) return false;
    if (std::fread(U.data(), 1, U.size(), fi) != U.size()) return false;
    if (std::fread(V.data(), 1, V.size(), fi) != V.size()) return false;
    const uint8_t *pl[3] = {Y.data(), U.data(), V.data()};
    const int stride[3] = {W, cw, cw};
    repack_planes(cfg, f, pl, stride);
    return true;
}

/* A frame with structure the codec has to work for: a low-frequency pattern
 * the DC-plane predictor can follow, an edge the directional modes can use, and
 * enough noise that the residual is never trivially zero.  Integer-only and
 * seeded by the frame number, so it is the same picture on every machine. */
void gen_frame(const Config &cfg, Frame &f, uint32_t frame_number) {
    const nxe_frame_params &fp = f.fp;
    const uint32_t rng = 0x9E3779B9u ^ (frame_number * 2246822519u);
    for (uint32_t t = 0; t < fp.ntiles; ++t) {
        const nxe_tile_job &j = f.jobs[t];
        for (int p = 0; p < NXE_MAX_PLANES; ++p) {
            const int size = f.plane_size[p];
            const int sub = (p && fp.chroma420) ? 2 : 1;
            const int ox = (int)j.col * (64 / sub), oy = (int)j.row * (64 / sub);
            int32_t *dst = &f.src[p][(size_t)t * size * size];
            /* Clamped exactly as read_frame clamps, so a frame that is not a
             * multiple of 64 is edge-replicated here too and the picture
             * round-trips through --dump-selftest-yuv unchanged. */
            const int pw = (int)fp.width / sub, ph = (int)fp.height / sub;
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    int gx = ox + x, gy = oy + y;
                    gx = gx >= pw ? pw - 1 : gx;
                    gy = gy >= ph ? ph - 1 : gy;
                    int v = 128 + ((gx * 3 + gy * 5 + (int)frame_number * 11) & 63) - 32;
                    if (((gx + (int)j.eye * 17) >> 4) % 3 == 0) v += 40;   /* edges */
                    if (p) v = 128 + ((v - 128) >> 2);
                    /* The noise is a function of position, not of iteration
                     * order, so a replicated edge sample is the same sample. */
                    uint32_t r = (uint32_t)(gx * 73856093) ^
                                 (uint32_t)(gy * 19349663) ^
                                 (uint32_t)((p + 1) * 83492791) ^ rng;
                    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                    v += (int)(r & 15) - 8;
                    dst[(size_t)y * size + x] = v < 0 ? 0 : (v > 255 ? 255 : v);
                }
            uint16_t *pk = &f.src_packed[(size_t)(f.plane_base[p] +
                                                  (int)t * f.plane_words[p]) * 2];
            for (int i = 0; i < size * size; ++i)
                pk[i] = (uint16_t)(int16_t)dst[i];
        }
    }
}

void fill_modes(const Config &cfg, Frame &f, uint32_t frame_number) {
    if (!cfg.intra_dir) return;
    uint32_t x = cfg.dir_mode_seed;
    if (x == 0) {
        std::fill(f.modes.begin(), f.modes.end(), (uint8_t)0);
        return;
    }
    x ^= frame_number * 2654435761u;
    for (size_t i = 0; i < f.modes.size(); ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;   /* xorshift32 */
        f.modes[i] = (uint8_t)(x % NXE_NUM_INTRA_MODES);
    }
}

/* ---------------------------------------------------------- stream header */
std::vector<uint8_t> stream_header(const Config &cfg, const Frame &f) {
    std::vector<uint8_t> b;
    auto u8 = [&](uint32_t v) { b.push_back((uint8_t)v); };
    auto u16 = [&](uint32_t v) { u8(v); u8(v >> 8); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) u8(v >> (8 * i)); };
    auto u64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) u8((uint32_t)(v >> (8 * i))); };
    uint64_t tools = 1ull << 0;                       /* INTRA_DC_PLANE */
    if (cfg.tskip) tools |= 1ull << 1;                /* TRANSFORM_SKIP */
    tools |= 1ull << 2;                               /* RES_LEVEL */
    if (cfg.chroma444) tools |= 1ull << 3;            /* CHROMA444 */
    if (cfg.nsub_log2 != 3) tools |= 1ull << 7;       /* NSUB_VAR */
    if (cfg.wm_id != 0) tools |= 1ull << 20;          /* WM_ID */
    if (cfg.intra_dir) tools |= 1ull << 17;           /* INTRA_DIR */
    if (cfg.ctx_v2) tools |= 1ull << 21;              /* CTX_V2 */
    if (cfg.ctx_v3) tools |= 1ull << 25;              /* CTX_V3 */
    if (cfg.sign_hide) tools |= 1ull << 22;           /* SIGN_HIDE */

    u32(0x3156584Eu);            /* 'NXV1' */
    u8(1);                       /* NXVC_VERSION */
    u8(1);                       /* profile */
    u8(1);                       /* level */
    u8(0);                       /* tile_size: 64x64 */
    u16(f.fp.width);
    u16(f.fp.height);
    u8(f.fp.eyes);
    u8(8);                       /* bit depth */
    u8(1);                       /* num_layers */
    u8(cfg.chroma444 ? 1 : 0);
    for (int i = 0; i < 4; ++i) u32(0);   /* layer_desc */
    u64(tools);
    u8(0);                       /* alpha */
    u8(0);                       /* color_transform */
    u8(0);                       /* color_space */
    while (b.size() < 62) u8(0);
    u16(0);                      /* TLV length */
    return b;
}

/* ------------------------------------------------------------ table sets
 * ref's count_units histogram plus table_set_cost / select_set, in the
 * reference's own double precision. */
/* The pool.
 *
 * It is a file static rather than a member of Frame so that two encoders -- a
 * WiVRn server runs one per eye -- share one set of threads instead of two,
 * and so that Frame stays the copyable aggregate the harness treats it as.
 * The threads are created on first use and live for the process; at frame rate
 * the alternative, spawning them per frame, is the larger cost.
 *
 * The pool is sized modestly on purpose.  This runs inside a compositor's
 * frame, next to its own submit thread and its own render work; taking every
 * core for four milliseconds of histogram would win this function and lose the
 * frame. */
namespace {

class TilePool {
public:
    static TilePool &get() {
        static TilePool pool;
        return pool;
    }

    unsigned width() const { return n_; }

    /* Runs `fn(t)` for t in [0, count), on `width()` threads, and returns when
     * every one has run. */
    void run(uint32_t count, const std::function<void(uint32_t)> &fn) {
        if (n_ <= 1 || count < 8) {
            for (uint32_t t = 0; t < count; ++t) fn(t);
            return;
        }
        std::unique_lock<std::mutex> outer(job_lock_);
        {
            std::lock_guard<std::mutex> lk(m_);
            fn_ = &fn;
            count_ = count;
            next_ = 0;
            done_ = 0;
            ++epoch_;
        }
        cv_.notify_all();
        /* The calling thread is one of the workers: it is going to wait
         * anyway, and on a two-core box it is the only worker there is. */
        work();
        std::unique_lock<std::mutex> lk(m_);
        cv_done_.wait(lk, [&] { return done_ == n_ - 1; });
        fn_ = nullptr;
    }

private:
    TilePool() {
        unsigned hw = std::thread::hardware_concurrency();
        n_ = hw ? std::min(8u, hw) : 1u;
        for (unsigned i = 1; i < n_; ++i)
            workers_.emplace_back([this] { loop(); });
    }
    ~TilePool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
            ++epoch_;
        }
        cv_.notify_all();
        for (auto &t : workers_) t.join();
    }

    void work() {
        for (;;) {
            uint32_t t = next_.fetch_add(1, std::memory_order_relaxed);
            if (t >= count_) return;
            (*fn_)(t);
        }
    }

    void loop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return stop_ || epoch_ != seen; });
                seen = epoch_;
                if (stop_) return;
            }
            work();
            {
                std::lock_guard<std::mutex> lk(m_);
                ++done_;
            }
            cv_done_.notify_one();
        }
    }

    unsigned n_ = 1;
    std::vector<std::thread> workers_;
    std::mutex m_, job_lock_;
    std::condition_variable cv_, cv_done_;
    const std::function<void(uint32_t)> *fn_ = nullptr;
    std::atomic<uint32_t> next_{0};
    uint32_t count_ = 0, done_ = 0;
    uint64_t epoch_ = 0;
    bool stop_ = false;
};

} // namespace

/* One tile's decision: the histogram E4 will produce, then the cheapest of the
 * eight candidate table sets under it. */
static void choose_tile_table_set(Frame &f, const int16_t *coefs, uint32_t t) {
    /* The op scratch is thread_local because the pool runs this on several
     * threads at once; it is a megabyte-class buffer that must not be
     * reallocated per tile. */
    thread_local std::vector<uint32_t> ops;
    ops.resize(NXE_UNIT_MAX_OPS);
    nxe_tile_units tu;
    nxe_build_units(&f.fp, &f.jobs[t], &tu);
    const int16_t *coef = &coefs[(size_t)t * NXE_TILE_COEFS_MAX];
    const uint8_t *modes = &f.modes[(size_t)t * 3 * 64];
    uint32_t hist[NXE_MAX_CTX][NXE_NUM_SYM];
    std::memset(hist, 0, sizeof hist);
    /* Every unit exactly once, but walked lane by lane rather than in
     * unit order: under v3 the context a unit codes in depends on the
     * neighbour class its own lane carries, so the histogram is only
     * right if the walk is the one E4 will actually perform.  The counts
     * themselves are order-independent; which row they land in is not. */
    for (int l = 0; l < tu.active; ++l) {
        nxe_nbr nbr = NXE_NBR_INIT;
        for (int ui = l; ui < tu.nunits; ui += tu.nlanes) {
            int n = nxe_unit_ops(&tu, ui, coef, modes, &nbr, ops.data());
            for (int i = 0; i < n; ++i)
                if (NXE_OP_KIND(ops[i]) == NXE_OP_SYM)
                    hist[NXE_OP_ARG(ops[i])][NXE_OP_VALUE(ops[i])]++;
        }
    }
    double best = 0;
    int bestk = (int)f.jobs[t].table_set;
    for (int k = 0; k < 8; ++k) {
        double bits = 0;
        /* `table_set_cost` sums over all kNumCtx rows, not over the coded
         * count: rows the model does not code have an empty histogram and
         * contribute nothing, so the two agree -- but only if this loop
         * has the same bound.  It is a hot loop over a 27-row table now.
         *
         * The log2 is hoisted into f.log_freq at build_tables() time: it
         * is a property of the table set, not of the tile, and calling it
         * per tile made this function the single most expensive thing on
         * the encode path once the picture stopped going through the
         * host.  Same doubles, same order, same sum. */
        const double *lf = &f.log_freq[(size_t)k * nxvc::kNumCtx * NXE_NUM_SYM];
        for (int c = 0; c < nxvc::kNumCtx; ++c)
            for (int s = 0; s < NXE_NUM_SYM; ++s)
                if (hist[c][s])
                    bits -= (double)hist[c][s] * lf[(size_t)c * NXE_NUM_SYM + s];
        if (k == 0 || bits < best) { best = bits; bestk = k; }
    }
    f.jobs[t].table_set = (uint32_t)bestk;
}

void choose_table_sets(Frame &f, const int16_t *coefs) {
    TilePool::get().run(f.fp.ntiles,
                        [&](uint32_t t) { choose_tile_table_set(f, coefs, t); });
}


/* -------------------------------------------------------------------- E5 */
void pack_frame(Frame &f, uint32_t frame_number) {
    const nxe_frame_params &fp = f.fp;
    uint32_t run = 0;
    for (uint32_t t = 0; t < fp.ntiles; ++t) {
        f.tile_prefix[t] = run;
        run += f.tile_bytes[t];
    }
    const uint32_t total = nxe_e5_frame_bytes(&fp, run);
    f.out.assign(total, 0);
    uint8_t pose[26];
    std::memset(pose, 0, sizeof pose);
    nxe_frame_params fp2 = fp;
    fp2.frame_number = frame_number;
    nxe_e5_frame_header(&fp2, pose, total, f.out.data());
    const uint32_t rowgroups = fp.tiles_y * fp.eyes;
    for (uint32_t g = 0; g < rowgroups; ++g) {
        uint32_t off = NXE_FRAME_HEADER_BYTES + NXE_ROW_HEADER_BYTES * g +
                       f.tile_prefix[g * fp.tiles_x];
        nxe_e5_row_header(&fp2, g, f.out.data() + off);
    }
    for (uint32_t t = 0; t < fp.ntiles; ++t) {
        uint32_t off = nxe_e5_tile_offset(&fp, t, f.tile_prefix.data());
        std::memcpy(f.out.data() + off,
                    &f.slots[(size_t)t * NXE_TILE_BYTES_MAX], f.tile_bytes[t]);
    }
}

void encode_frame_cpu(Frame &f, uint32_t frame_number) {
    const nxe_frame_params &fp = f.fp;
    for (uint32_t t = 0; t < fp.ntiles; ++t) {
        const int32_t *src[NXE_MAX_PLANES];
        for (int p = 0; p < NXE_MAX_PLANES; ++p)
            src[p] = &f.src[p][(size_t)t * f.plane_size[p] * f.plane_size[p]];
        nxe_e3_tile(&fp, &f.jobs[t], src, &f.modes[(size_t)t * 3 * 64],
                    &f.coef[(size_t)t * NXE_TILE_COEFS_MAX]);
    }
    choose_table_sets(f, f.coef.data());
    for (uint32_t t = 0; t < fp.ntiles; ++t) {
        nxe_tile_units tu;
        nxe_build_units(&fp, &f.jobs[t], &tu);
        int len = nxe_e4_tile(&fp, &f.jobs[t], &tu,
                              &f.coef[(size_t)t * NXE_TILE_COEFS_MAX],
                              &f.modes[(size_t)t * 3 * 64], &f.tabs,
                              &f.slots[(size_t)t * NXE_TILE_BYTES_MAX]);
        f.jobs[t].payload_len = (uint32_t)len;
        f.tile_bytes[t] = (uint32_t)(NXE_TILE_HEADER_BYTES + len);
    }
    pack_frame(f, frame_number);
}

}  // namespace nxe
