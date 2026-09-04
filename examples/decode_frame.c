/* decode_frame.c -- the smallest useful NX Warp decoder.
 *
 *   A .nxv file in, raw planar 8-bit YUV out.  Plain C, C ABI only.
 *
 * Run:
 *   nxvc-example-decode --in out.nxv --out out.yuv [--frames N]
 *
 * The shape of every NX Warp decoder, GPU ones included:
 *
 *   1. nxvc_decoder_create()
 *   2. nxvc_decoder_parse_stream_header()  once, consumes 64 + ext_len bytes
 *   3. nxvc_decoder_stream_info()          geometry comes from the STREAM,
 *                                          never from the caller's command line
 *   4. nxvc_decoder_plane_size()           allocate the output planes
 *   5. nxvc_decoder_decode_frame()         in a loop, each call reporting how
 *                                          many bytes of the file it consumed
 *
 * Point 3 is the part people get wrong.  `--pix yuv420p` on a decoder command
 * line is a *check*, not an input: the stream already says what it is.  This
 * example has no --pix at all, and prints what it found instead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nxvc/nxvc.h>

static void usage(void) {
    fprintf(stderr, "usage: nxvc-example-decode --in FILE.nxv --out FILE.yuv [--frames N]\n");
}

/* Slurp the whole bitstream.  A real decoder streams; an example that fits on
 * one screen does not, and .nxv frames are self-delimiting either way. */
static uint8_t *read_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(buf); return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    const char *in_path = NULL, *out_path = NULL;
    uint32_t want_frames = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
#define ARG(name) (strcmp(a, name) == 0 && v && ++i)
        if (ARG("--in")) in_path = v;
        else if (ARG("--out")) out_path = v;
        else if (ARG("--frames")) want_frames = (uint32_t)strtoul(v, NULL, 10);
        else { usage(); return 2; }
#undef ARG
    }
    if (!in_path || !out_path) { usage(); return 2; }

    size_t len = 0;
    uint8_t *data = read_all(in_path, &len);
    if (!data) return 1;

    nxvc_status st;
    nxvc_decoder *dec = nxvc_decoder_create(&st);
    if (!dec) {
        fprintf(stderr, "decoder_create: %s\n", nxvc_status_string(st));
        free(data);
        return 1;
    }

    /* 2. Stream header. */
    size_t off = 0, consumed = 0;
    st = nxvc_decoder_parse_stream_header(dec, data, len, &consumed);
    if (st != NXVC_OK) {
        /* NXVC_ERR_VERSION here means "this is not an NX Warp v1 stream, or it
         * uses a tool this build does not implement" -- the honest failure of a
         * Phase 1 decoder handed a Phase 2 stream. */
        fprintf(stderr, "stream header: %s\n", nxvc_status_string(st));
        goto fail;
    }
    off += consumed;

    /* 3. What did we just open? */
    nxvc_stream_info si;
    nxvc_decoder_stream_info(dec, &si);
    printf("%s: %ux%u %s%s, %u eye(s), %u-bit, tools 0x%llx\n", in_path,
           si.width, si.height,
           si.chroma == NXVC_CHROMA_444 ? "4:4:4" : "4:2:0",
           si.alpha ? " + alpha" : "", si.eyes, si.bit_depth,
           (unsigned long long)si.tools);
    if (si.ext_unknown_count)
        printf("  (%u unknown extension TLV%s, skipped -- forward compatibility "
               "working as designed)\n",
               si.ext_unknown_count, si.ext_unknown_count == 1 ? "" : "s");

    /* 4. Output planes, sized by the decoder, not by us. */
    {
        uint32_t pw[4] = {0}, ph[4] = {0};
        int nplanes = si.alpha ? 4 : 3;
        uint8_t *planes[4] = {NULL, NULL, NULL, NULL};
        nxvc_image img;
        memset(&img, 0, sizeof img);
        for (int p = 0; p < nplanes; p++) {
            if (nxvc_decoder_plane_size(dec, p, &pw[p], &ph[p]) != NXVC_OK) {
                fprintf(stderr, "plane_size(%d) failed\n", p);
                goto fail;
            }
            planes[p] = malloc((size_t)pw[p] * ph[p]);
            if (!planes[p]) { fprintf(stderr, "out of memory\n"); goto fail; }
            img.plane[p] = planes[p];
            img.stride[p] = (int32_t)pw[p];
        }

        FILE *fout = fopen(out_path, "wb");
        if (!fout) { perror(out_path); goto fail; }

        /* 5. Frame loop.  `consumed` is how the caller walks the file: frames
         *    are variable length and only the decoder knows where each ends. */
        uint32_t n = 0;
        while (off < len) {
            if (want_frames && n >= want_frames) break;
            st = nxvc_decoder_decode_frame(dec, data + off, len - off, &img, &consumed);
            if (st != NXVC_OK) {
                fprintf(stderr, "frame %u at offset %zu: %s\n", n, off,
                        nxvc_status_string(st));
                fclose(fout);
                goto fail;
            }
            for (int p = 0; p < nplanes; p++)
                fwrite(planes[p], 1, (size_t)pw[p] * ph[p], fout);
            off += consumed;
            n++;
        }
        fclose(fout);

        printf("decoded %u frame%s to %s\n", n, n == 1 ? "" : "s", out_path);
        printf("  plane sizes:");
        for (int p = 0; p < nplanes; p++) printf(" %ux%u", pw[p], ph[p]);
        printf("\n  play it back with:\n"
               "    ffplay -f rawvideo -pixel_format %s -video_size %ux%u %s\n",
               si.chroma == NXVC_CHROMA_444 ? "yuv444p" : "yuv420p",
               si.width, si.height, out_path);

        for (int p = 0; p < 4; p++) free(planes[p]);
        nxvc_decoder_destroy(dec);
        free(data);
        return 0;
    }

fail:
    nxvc_decoder_destroy(dec);
    free(data);
    return 1;
}
