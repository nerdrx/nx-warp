/* nxe_host.h -- the frame driver shared by the CPU models and the GPU pipeline.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Everything here is host work that is *not* one of the kernels: reading a
 * planar YUV frame into the tile-major layout E0 produces, building the frame
 * parameter record and the per-tile job list, choosing each tile's probability
 * table set, and assembling the stream header.  The kernels themselves are
 * `forward_cpu.h` / `rans_cpu.h` and their `.comp` counterparts.
 *
 * The table-set choice is host work on purpose.  `select_set` in the reference
 * encoder minimises a sum of `log2`s over eight candidate tables; that is a
 * floating-point decision, it is not normative, and paper 3.6 has E1 and the
 * rate controller settling the per-tile parameters anyway.  Reproducing the
 * reference's choice exactly -- in the reference's own double precision -- is
 * what lets `--check` compare bitstreams byte for byte instead of merely
 * comparing decoded pixels.
 */

#ifndef NXE_HOST_H
#define NXE_HOST_H

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "forward_cpu.h"
#include "nxe_enc.h"
#include "rans_cpu.h"
}

namespace nxe {

struct Config {
    std::string in, out;
    int w = 0, h = 0;
    int eyes = 1;
    bool chroma444 = false;
    int qp = 24;
    int frames = -1;
    int matrix = 1;
    int wm_id = 0;
    int chroma_qp_off = 0;
    int nsub_log2 = 3;
    int tskip = 0;
    bool ctx_v2 = true;
    /* Tool bit 25.  v3 is a refinement of v2, never a replacement -- the
     * stream header refuses bit 25 without bit 21 -- so setting this implies
     * ctx_v2.  Off by default, as `nxv-enc --ctx v3` is. */
    bool ctx_v3 = false;
    bool sign_hide = true;
    bool intra_dir = false;
    bool dir_layer = false;
    /* Directional intra takes its per-block modes as an input (the search is a
     * host or E1 job, see forward_cpu.h).  A nonzero seed fills them from a
     * deterministic PRNG so the tests exercise all nine modes; 0 leaves every
     * block on mode 0, which reproduces the v1 predictor exactly. */
    uint32_t dir_mode_seed = 0;
    int device = 0;
    bool cpu_only = false;
    bool bench = false;
    int bench_iters = 50;
    bool quiet = false;
};

/* Per-frame host state, allocated once. */
struct Frame {
    nxe_frame_params fp{};
    std::vector<nxe_tile_job> jobs;
    /* Source planes in the tile-major layout of vk/encoder/README.md: one
     * 64x64 (or 32x32 chroma) tile is a contiguous run.  int32 here because
     * the CPU models want it; the GPU buffer is the packed int16 form. */
    std::vector<int32_t> src[NXE_MAX_PLANES];
    std::vector<uint16_t> src_packed;      /* what E0 would have written */
    std::vector<int16_t> coef;             /* ntiles * NXE_TILE_COEFS_MAX */
    std::vector<uint8_t> modes;            /* ntiles * 3 * 64 */
    std::vector<uint8_t> slots;            /* ntiles * NXE_TILE_SLOT_BYTES */
    std::vector<uint32_t> tile_bytes, tile_prefix;
    std::vector<uint8_t> out;              /* the assembled frame */
    nxe_tables tabs{};
    /* log2(freq / 1024) for every (table set, context, symbol) of `tabs`,
     * which is the only thing choose_table_sets() does with the tables and is
     * constant for the life of the stream.  Computing it once turns that
     * function's inner loop from a few thousand std::log2 calls per tile into
     * a multiply-add -- the same doubles, in the same order, so the sum and
     * therefore the chosen table set are unchanged bit for bit. */
    std::vector<double> log_freq;
    int plane_size[NXE_MAX_PLANES]{};
    int plane_words[NXE_MAX_PLANES]{};     /* tile stride in the packed buffer */
    int plane_base[NXE_MAX_PLANES]{};      /* word base of the plane */
};

/* Set up geometry, jobs and the frame parameter record. */
void setup(const Config &cfg, Frame &f);

/* Move a Frame that setup() already built to another quantiser, without
 * rebuilding anything.
 *
 * This is the whole of what the QP touches on this path, which is why it can
 * be a four-line function: the geometry, the job list, the probability tables,
 * the weighting matrices and the stream header are all independent of it.  The
 * two things that are not are `fp.base_qp` -- carried in the frame header and
 * read by E3 -- and the per-tile table-set SEED, which choose_table_sets()
 * overwrites from the coefficients before E4 ever reads it and which is set
 * here only so that a Frame at QP q is indistinguishable from one setup() just
 * built at QP q.  Both buffers are re-uploaded every frame (see nxe_vk.cpp),
 * so there is nothing to invalidate.
 *
 * setup() itself goes through this function, so the two cannot drift. */
void set_qp(Config &cfg, Frame &f, int qp);

/* Fill the built-in probability tables for the frame's context model. */
void build_tables(const Config &cfg, Frame &f);

/* Lay three caller-owned planar 8-bit planes out tile-major, exactly as
 * read_frame does -- it is read_frame with the file read hoisted out, so the
 * two can never drift.  This is the CPU stand-in for E0 on the library path,
 * where the frame arrives as pointers rather than as a file. */
void load_planes(const Config &cfg, Frame &f,
                 const uint8_t *y, size_t y_stride,
                 const uint8_t *cb, const uint8_t *cr, size_t chroma_stride);

/* Read one frame of planar 8-bit YUV and lay it out tile-major.  Returns false
 * at end of file. */
bool read_frame(std::FILE *fi, const Config &cfg, Frame &f);

/* Synthesize one deterministic frame directly into `f`, bypassing the file
 * reader.  Used by --selftest so the suite needs no test vectors on disk. */
void gen_frame(const Config &cfg, Frame &f, uint32_t frame_number);

/* Fill the per-block intra mode array from cfg.dir_mode_seed. */
void fill_modes(const Config &cfg, Frame &f, uint32_t frame_number);

/* The 64-byte stream header, ref's nxvc_encoder_stream_header. */
std::vector<uint8_t> stream_header(const Config &cfg, const Frame &f);

/* Per-tile table set from the coefficients, reproducing ref's select_set.
 *
 * `coef` is the coefficient array, ntiles * NXE_TILE_COEFS_MAX entries.  It is
 * a parameter rather than `f.coef` because on the GPU path the coefficients
 * are already in a host-cached mapping of the readback buffer, and copying
 * seven megabytes into `f.coef` first only to read them once is a copy for
 * nobody.  Pass `f.coef.data()` on the CPU path.
 *
 * Every tile's decision is independent -- it reads that tile's coefficients
 * and modes and writes that tile's job -- so the work is split across a small
 * pool of threads.  Which thread does which tile changes nothing: each sum is
 * over one tile's own histogram in a fixed order, so the chosen set, and the
 * bitstream, are the same as the serial walk's. */
void choose_table_sets(Frame &f, const int16_t *coef);

/* E5 on the host: lay the tile slots out into `f.out`. */
void pack_frame(Frame &f, uint32_t frame_number);

/* The whole CPU pipeline for one frame: E3, table sets, E4, E5. */
void encode_frame_cpu(Frame &f, uint32_t frame_number);

}  // namespace nxe

#endif /* NXE_HOST_H */
