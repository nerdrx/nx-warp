// Range safety of the normative decode path.
//
// Every product in the inverse transform must stay inside int32 for every
// input a legal bitstream can produce.  Dequantized coefficients are clamped
// to int16 (SYNTAX.md 6.5), so the reachable input space of idct8x8 is exactly
// [-32768, 32767]^64 -- but the *internal* nodes are not bounded by the
// coefficients: the odd-part rotation operand `P +- Q` reaches +-8.6e7, and
// multiplying that by C4 = 362 leaves int32.  SYNTAX.md 6.3 specifies the
// rotation as an exact two-word product for that reason.
//
// This test drives the arithmetic at its documented worst case and then
// decodes the saturating conformance vector, which carries the same pattern
// through the real decoder.  Built and run under -fsanitize=undefined
// (cmake --preset asan-ubsan) it is the check that the normative path has no
// signed overflow anywhere.
#include "test_util.h"
#include "common.h"
#include "transform.h"
#include "nxvc/nxvc.h"

using namespace nxvc;

// The four odd-frequency signs that maximise |P + Q| in the inverse flow
// graph: A and C pull apart, B and D add.
static void worst_odd(i32 c[64], int sgn) {
    for (int i = 0; i < 64; ++i) c[i] = 0;
    for (int r = 0; r < 8; ++r) {
        c[r * 8 + 1] = sgn ? -32768 : 32767;
        c[r * 8 + 3] = sgn ? 32767 : -32768;
        c[r * 8 + 5] = sgn ? -32768 : 32767;
        c[r * 8 + 7] = sgn ? 32767 : -32768;
    }
}

// The 4x4 graph's worst case: every coefficient at the int16 clamp with the
// sign pattern that maximises |t0| + |t3| at the outputs.
static void worst_4x4(i32 c[16], int sgn) {
    for (int r = 0; r < 4; ++r) {
        c[r * 4 + 0] = sgn ? -32768 : 32767;
        c[r * 4 + 1] = sgn ? -32768 : 32767;
        c[r * 4 + 2] = sgn ? 32767 : -32768;
        c[r * 4 + 3] = sgn ? 32767 : -32768;
    }
}

int main(int argc, char **argv) {
    // 0. The 4x4 transform of SYNTAX.md 6.7 at its documented bounds.  Its
    //    products are small enough not to need the two-word rotation, and this
    //    is the test that says so: run under -fsanitize=undefined, a signed
    //    overflow here aborts.
    {
        i32 c[16], out[16];
        for (int sgn = 0; sgn < 2; ++sgn) {
            worst_4x4(c, sgn);
            idct4x4(c, out);
            for (int i = 0; i < 16; ++i)
                CHECK(out[i] >= -32768 && out[i] <= 32767, "worst_4x4[%d]=%d",
                      i, out[i]);
        }
        // every single-coefficient extreme, both signs
        for (int k = 0; k < 16; ++k)
            for (int sgn = 0; sgn < 2; ++sgn) {
                for (int i = 0; i < 16; ++i) c[i] = 0;
                c[k] = sgn ? -32768 : 32767;
                idct4x4(c, out);
                for (int i = 0; i < 16; ++i)
                    CHECK(out[i] >= -32768 && out[i] <= 32767,
                          "single 4x4 %d/%d [%d]=%d", k, sgn, i, out[i]);
            }
        // and a sweep of random int16 blocks
        Rng rng(77);
        for (int it = 0; it < 20000; ++it) {
            for (int i = 0; i < 16; ++i) c[i] = rng.range(-32768, 32767);
            idct4x4(c, out);
            for (int i = 0; i < 16; ++i)
                CHECK(out[i] >= -32768 && out[i] <= 32767, "rand 4x4 [%d]=%d",
                      i, out[i]);
        }
    }

    // 1. The worst case for the odd-part rotation, both signs.
    {
        i32 c[64], out[64];
        for (int sgn = 0; sgn < 2; ++sgn) {
            worst_odd(c, sgn);
            idct8x8(c, out);
            for (int i = 0; i < 64; ++i)
                CHECK(out[i] >= -32768 && out[i] <= 32767, "worst_odd[%d]=%d", i,
                      out[i]);
        }
    }

    // 2. Every single-coefficient extreme, both signs: this walks each input of
    //    the flow graph to its bound on its own.
    {
        for (int k = 0; k < 64; ++k)
            for (int sgn = 0; sgn < 2; ++sgn) {
                i32 c[64] = {0}, out[64];
                c[k] = sgn ? -32768 : 32767;
                idct8x8(c, out);
                for (int i = 0; i < 64; ++i)
                    CHECK(out[i] >= -32768 && out[i] <= 32767,
                          "single[%d] out[%d]=%d", k, i, out[i]);
            }
    }

    // 3. Sign-pattern sweep: all 256 patterns of the eight frequencies of a
    //    row, replicated down the block, at the int16 bound.
    {
        for (int mask = 0; mask < 256; ++mask) {
            i32 c[64], out[64];
            for (int r = 0; r < 8; ++r)
                for (int v = 0; v < 8; ++v)
                    c[r * 8 + v] = (mask >> v) & 1 ? -32768 : 32767;
            idct8x8(c, out);
            for (int i = 0; i < 64; ++i)
                CHECK(out[i] >= -32768 && out[i] <= 32767, "mask %d out[%d]=%d",
                      mask, i, out[i]);
        }
    }

    // 4. Random int16 coefficients, a wide sweep.
    {
        Rng rng(0xC0FFEE);
        for (int it = 0; it < 20000; ++it) {
            i32 c[64], out[64];
            for (int i = 0; i < 64; ++i) c[i] = rng.range(-32768, 32767);
            idct8x8(c, out);
            for (int i = 0; i < 64; ++i)
                CHECK(out[i] >= -32768 && out[i] <= 32767, "rand out[%d]=%d", i,
                      out[i]);
        }
    }

    // 5. The dequantizer at its own bound: the largest step (QP 63 with the
    //    largest legal weight) times the largest legal level.  q * t must stay
    //    in int32 and the result must land inside the int16 clamp.
    {
        const int t = (kQStep[63] * 32 + 8) >> 4;
        CHECK(t == 46340, "max dequant step %d", t);
        for (int sgn = 0; sgn < 2; ++sgn) {
            i32 q = sgn ? -32767 : 32767;
            i32 v = clamp16((q * t + 8) >> 4);
            CHECK(v >= -32768 && v <= 32767, "dequant %d", v);
        }
        // and the full chain: saturated coefficients through the transform.
        i32 c[64], out[64];
        for (int i = 0; i < 64; ++i)
            c[i] = clamp16(((i & 1 ? -32767 : 32767) * t + 8) >> 4);
        idct8x8(c, out);
        for (int i = 0; i < 64; ++i)
            CHECK(out[i] >= -32768 && out[i] <= 32767, "chain out[%d]=%d", i,
                  out[i]);
    }

    // 6. The saturating conformance vector through the real decoder.
    if (argc > 1) {
        std::string path = std::string(argv[1]) + "/v35_saturate420.nxv";
        std::FILE *f = std::fopen(path.c_str(), "rb");
        CHECK(f != nullptr, "missing %s", path.c_str());
        if (f) {
            std::fseek(f, 0, SEEK_END);
            long fsz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::vector<uint8_t> data((size_t)(fsz > 0 ? fsz : 0));
            size_t rd = std::fread(data.data(), 1, data.size(), f);
            std::fclose(f);
            CHECK(rd == data.size(), "short read");
            nxvc_status st;
            nxvc_decoder *d = nxvc_decoder_create(&st);
            size_t consumed = 0;
            st = nxvc_decoder_parse_stream_header(d, data.data(), data.size(),
                                                  &consumed);
            CHECK(st == NXVC_OK, "header: %s", nxvc_status_string(st));
            uint32_t yw, yh, cw, ch;
            nxvc_decoder_plane_size(d, 0, &yw, &yh);
            nxvc_decoder_plane_size(d, 1, &cw, &ch);
            std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch),
                V((size_t)cw * ch);
            nxvc_image oi{};
            oi.plane[0] = Y.data(); oi.stride[0] = (int)yw;
            oi.plane[1] = U.data(); oi.stride[1] = (int)cw;
            oi.plane[2] = V.data(); oi.stride[2] = (int)cw;
            size_t off = consumed;
            st = nxvc_decoder_decode_frame(d, data.data() + off,
                                           data.size() - off, &oi, &consumed);
            CHECK(st == NXVC_OK, "decode: %s", nxvc_status_string(st));
            nxvc_decoder_destroy(d);
        }
    }

    return test_report("test_saturate");
}
