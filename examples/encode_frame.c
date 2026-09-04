/* encode_frame.c -- the smallest useful NX Warp encoder.
 *
 *   Raw planar 8-bit YUV in, a .nxv bitstream out, using nothing but the C ABI
 *   in <nxvc/nxvc.h>.  This is deliberately plain C: the ABI is a C ABI, and if
 *   this file needs a C++ compiler then the ABI has a bug.
 *
 * Build:
 *   cmake -B build -G Ninja -DNXWARP_BUILD_EXAMPLES=ON && cmake --build build
 *
 * Run:
 *   nxvc-example-encode --in in.yuv --w 1024 --h 512 --pix yuv444p \
 *                       --qp 28 --frames 4 --out out.nxv
 *
 * The five calls that matter, in order:
 *
 *   1. nxvc_config_default()          fill a config with legal values
 *   2. nxvc_encoder_create()          allocate the encoder for that geometry
 *   3. nxvc_encoder_stream_header()   write the 64-byte stream header ONCE
 *   4. nxvc_encoder_encode_frame()    once per frame, appended to the same file
 *   5. nxvc_encoder_destroy()
 *
 * The stream header is written once and the frames are concatenated after it.
 * A .nxv file is exactly `stream_header ext_area frame*` -- see ref/README.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nxvc/nxvc.h>

/* ------------------------------------------------------------------ helpers */

/* Bytes in one plane of a frame, for the chroma format the config asks for.
 * plane 0 is full resolution; planes 1 and 2 are half in each dimension for
 * 4:2:0 (rounded up, because odd sizes are legal); plane 3 (alpha) is full. */
static size_t plane_bytes(uint32_t w, uint32_t h, int plane, int chroma420) {
    if (plane == 0 || plane == 3) return (size_t)w * h;
    if (chroma420) return (size_t)((w + 1) / 2) * ((h + 1) / 2);
    return (size_t)w * h;
}

static void usage(void) {
    fprintf(stderr,
            "usage: nxvc-example-encode --in FILE --w W --h H "
            "[--pix yuv420p|yuv444p] [--qp 0..63] [--frames N] --out FILE\n");
}

/* ---------------------------------------------------------------------- main */

int main(int argc, char **argv) {
    const char *in_path = NULL, *out_path = NULL, *pix = "yuv420p";
    uint32_t w = 0, h = 0, qp = 28, want_frames = 0; /* 0 = to end of file */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
#define ARG(name) (strcmp(a, name) == 0 && v && ++i)
        if (ARG("--in")) in_path = v;
        else if (ARG("--out")) out_path = v;
        else if (ARG("--pix")) pix = v;
        else if (ARG("--w")) w = (uint32_t)strtoul(v, NULL, 10);
        else if (ARG("--h")) h = (uint32_t)strtoul(v, NULL, 10);
        else if (ARG("--qp")) qp = (uint32_t)strtoul(v, NULL, 10);
        else if (ARG("--frames")) want_frames = (uint32_t)strtoul(v, NULL, 10);
        else { usage(); return 2; }
#undef ARG
    }
    if (!in_path || !out_path || !w || !h) { usage(); return 2; }

    int chroma420 = (strcmp(pix, "yuv420p") == 0);
    if (!chroma420 && strcmp(pix, "yuv444p") != 0) {
        fprintf(stderr, "--pix must be yuv420p or yuv444p\n");
        return 2;
    }

    /* 1. Config.  ALWAYS start from nxvc_config_default(): the struct grows
     *    between versions and a zeroed config is not a legal one. */
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = w;
    cfg.height = h;
    cfg.chroma = chroma420 ? NXVC_CHROMA_420 : NXVC_CHROMA_444;
    cfg.base_qp = qp;
    /* cfg.color_transform stays NXVC_CT_NONE: the input planes are already
     * Y/Cb/Cr and are coded as given.  Set NXVC_CT_YCOCGR only when the three
     * planes are R, G, B and you want the codec's own reversible transform. */

    /* 2. Encoder. */
    nxvc_status st;
    nxvc_encoder *enc = nxvc_encoder_create(&cfg, &st);
    if (!enc) {
        fprintf(stderr, "encoder_create: %s\n", nxvc_status_string(st));
        return 1;
    }

    FILE *fin = fopen(in_path, "rb");
    FILE *fout = fopen(out_path, "wb");
    if (!fin || !fout) {
        perror(fin ? out_path : in_path);
        nxvc_encoder_destroy(enc);
        if (fin) fclose(fin);
        if (fout) fclose(fout);
        return 1;
    }

    /* 3. Stream header.  64 bytes plus any TLV extension area; ask for enough
     *    room and use the returned length.  Written exactly once. */
    uint8_t hdr[512];
    size_t hdr_len = 0;
    st = nxvc_encoder_stream_header(enc, hdr, sizeof hdr, &hdr_len);
    if (st != NXVC_OK) {
        fprintf(stderr, "stream_header: %s\n", nxvc_status_string(st));
        goto fail;
    }
    fwrite(hdr, 1, hdr_len, fout);

    /* Input buffers.  Planes are separate allocations so the strides can be
     * anything; here they are tight. */
    {
        size_t ysz = plane_bytes(w, h, 0, chroma420);
        size_t csz = plane_bytes(w, h, 1, chroma420);
        size_t frame_sz = ysz + 2 * csz;

        uint8_t *py = malloc(ysz), *pu = malloc(csz), *pv = malloc(csz);
        /* A worst-case frame is bounded by the uncompressed size plus headers;
         * being generous here costs one allocation and removes a whole class of
         * NXVC_ERR_NOMEM surprises. */
        size_t cap = frame_sz + frame_sz / 2 + (1u << 16);
        uint8_t *bs = malloc(cap);
        if (!py || !pu || !pv || !bs) {
            fprintf(stderr, "out of memory\n");
            goto fail;
        }

        nxvc_image img;
        memset(&img, 0, sizeof img);
        img.plane[0] = py; img.stride[0] = (int32_t)w;
        img.plane[1] = pu; img.stride[1] = (int32_t)(chroma420 ? (w + 1) / 2 : w);
        img.plane[2] = pv; img.stride[2] = img.stride[1];
        /* img.plane[3] stays NULL: cfg.alpha is 0. */

        uint32_t n = 0;
        uint64_t total = 0;
        for (;;) {
            if (want_frames && n >= want_frames) break;
            if (fread(py, 1, ysz, fin) != ysz) break; /* clean EOF */
            if (fread(pu, 1, csz, fin) != csz || fread(pv, 1, csz, fin) != csz) {
                fprintf(stderr, "truncated frame %u in %s\n", n, in_path);
                goto fail;
            }

            /* 4. Encode.  qp_map and res_map are per-tile byte arrays in raster
             *    order (see tile_walk.c for how to build one); NULL means "use
             *    base_qp everywhere, res_level 0 everywhere". */
            size_t len = 0;
            st = nxvc_encoder_encode_frame(enc, &img, NULL, NULL, bs, cap, &len);
            if (st != NXVC_OK) {
                fprintf(stderr, "encode_frame %u: %s\n", n, nxvc_status_string(st));
                goto fail;
            }
            fwrite(bs, 1, len, fout);
            total += len;
            n++;
        }

        fclose(fin);
        fclose(fout);
        nxvc_encoder_destroy(enc);
        free(py); free(pu); free(pv); free(bs);

        if (n == 0) {
            fprintf(stderr, "no complete frames read from %s\n", in_path);
            return 1;
        }
        printf("encoded %u frame%s, %llu bytes of frame data + %zu byte header\n",
               n, n == 1 ? "" : "s", (unsigned long long)total, hdr_len);
        printf("  %.2f bits/pixel, %.1f kB/frame\n",
               (double)(total * 8) / ((double)n * w * h),
               (double)total / n / 1000.0);
        return 0;
    }

fail:
    if (fin) fclose(fin);
    if (fout) fclose(fout);
    nxvc_encoder_destroy(enc);
    return 1;
}
