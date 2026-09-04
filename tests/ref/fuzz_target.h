// The single decode entry point shared by the smoke test and the libFuzzer
// target, so both exercise exactly the same code path.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

#include "nxvc/nxvc.h"

// Feeds `data` to the decoder as a whole stream: header, then frames until the
// buffer is exhausted or a frame is rejected.  Never asserts.
inline int nxvc_fuzz_decode(const uint8_t *data, size_t size) {
    nxvc_status st;
    nxvc_decoder *d = nxvc_decoder_create(&st);
    if (!d) return 0;
    size_t consumed = 0;
    st = nxvc_decoder_parse_stream_header(d, data, size, &consumed);
    if (st != NXVC_OK) {
        nxvc_decoder_destroy(d);
        return 0;
    }
    uint32_t yw = 0, yh = 0, cw = 0, ch = 0;
    nxvc_decoder_plane_size(d, 0, &yw, &yh);
    nxvc_decoder_plane_size(d, 1, &cw, &ch);
    // Guard against absurd allocations from a hostile header.
    if ((uint64_t)yw * yh > (uint64_t)(1u << 24)) {
        nxvc_decoder_destroy(d);
        return 0;
    }
    std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch),
        V((size_t)cw * ch), A((size_t)yw * yh);
    nxvc_image img{};
    img.plane[0] = Y.data(); img.stride[0] = (int)yw;
    img.plane[1] = U.data(); img.stride[1] = (int)cw;
    img.plane[2] = V.data(); img.stride[2] = (int)cw;
    img.plane[3] = A.data(); img.stride[3] = (int)yw;
    size_t off = consumed;
    int frames = 0;
    while (off < size && frames < 64) {
        size_t used = 0;
        st = nxvc_decoder_decode_frame(d, data + off, size - off, &img, &used);
        if (st != NXVC_OK || used == 0) break;
        off += used;
        ++frames;
    }
    nxvc_decoder_destroy(d);
    return 0;
}
