#!/usr/bin/env python3
"""Generate the seed corpora under fuzz/corpus/.

A seed corpus does two jobs: it gets the fuzzer past the parts of the format
that are cheap to check and expensive to guess (magic numbers, fixed-size
headers, per-lane rANS initial states), and it pins the *shape* the custom
mutator will then explore.  Seeds are therefore small and structural, not
"interesting" -- the interesting inputs are what the fuzzer produces from them.

Real conformance vectors from tests/vectors/ are copied in where they are small
enough to be worth carrying in git; they are the only inputs in the tree that
are valid all the way down to the coefficient level.

Run:  python3 fuzz/tools/gen_corpus.py --repo <repo root> --out fuzz/corpus
      (or: cmake --build <build> --target nxfuzz-corpus)
"""

import argparse
import hashlib
import os
import shutil
import struct

# Keep the checked-in corpus small; libFuzzer will grow its own working corpus.
MAX_VECTOR_BYTES = 8192
MAX_VECTORS = 8

# Vectors carried whatever the size cap and the count cap say, because they are
# the only inputs in the tree that reach a syntax the sorted-order sweep above
# would never get to: a tool bit the decoder has just learned, and the per-tile
# fields behind it.  A fuzzer cannot invent a valid stream with tool bit 24 set;
# it can mutate one.  Keep this list to the newest syntax revision.
ALWAYS_VECTORS = (
    "v57_near_skip.nxv",       # 13.9  near-skip, DC form
    "v58_quad_mv.nxv",         # 13.10 quadrant vectors
    "v59_near_skip_420.nxv",   # 13.9  near-skip at nb == 4
    "v60_inter_eff_all.nxv",   # 13.8 + 13.9 + 13.10 together
    "v61_sub_intra.nxv",       # 13.11 sub-tile intra
)

TARGETS = [
    "nxvc_decode_fuzz",
    "nxvc_headers_fuzz",
    "nxvc_rans_fuzz",
    "transport_depacketize_fuzz",
    "transport_rs_fuzz",
    "transport_feedback_fuzz",
    "warp_tile_fuzz",
]


def write(outdir, name, data):
    path = os.path.join(outdir, name)
    with open(path, "wb") as f:
        f.write(data)
    return path


def digest(data):
    return hashlib.sha1(data).hexdigest()[:12]


# ---------------------------------------------------------------- nxvc stream
MAGIC = 0x3156584E


def stream_header(width, height, chroma=0, tools=0x1, alpha=0, ct=0, ext=b""):
    h = bytearray(64)
    struct.pack_into("<I", h, 0, MAGIC)
    h[4] = 1  # version
    h[5] = 1  # profile
    h[6] = 0  # level
    h[7] = 0  # tile_size 64x64
    struct.pack_into("<H", h, 8, width)
    struct.pack_into("<H", h, 10, height)
    h[12] = 1  # eyes
    h[13] = 8  # bit_depth
    h[14] = 1  # num_layers
    h[15] = chroma
    struct.pack_into("<Q", h, 32, tools)
    h[40] = alpha
    h[41] = ct
    struct.pack_into("<H", h, 62, len(ext))
    return bytes(h) + ext


def tile(index, nsub_log2=3, res_level=0, tskip=0, payload=b""):
    w0 = (0 << 0) | (0 << 2) | ((index & 0xFFF) << 4) | ((len(payload) & 0xFFFF) << 16)
    w1 = (3 << 0)  # INTRA
    w1 |= (res_level & 3) << 3
    w1 |= (0 & 7) << 14  # table_set
    w1 |= (nsub_log2 & 7) << 17
    w1 |= (tskip & 1) << 23
    return struct.pack("<II", w0, w1) + payload


def tile_row(frame_number, row_index, tiles):
    return struct.pack("<HBBQ", frame_number, row_index, len(tiles), 0) + b"".join(tiles)


def frame(frame_number, base_qp, rows, quant_matrix=1, tables_present=0, flags=1):
    body = b"".join(rows)
    h = bytearray(40)
    struct.pack_into("<H", h, 0, frame_number)
    h[28] = base_qp
    h[31] = quant_matrix
    h[32] = tables_present
    h[34] = flags
    struct.pack_into("<I", h, 36, 40 + len(body))
    return bytes(h) + body


def rans_payload(nlanes, extra=64):
    """4 bytes of initial state per lane (each >= 2^16), then renorm pairs."""
    out = bytearray()
    for lane in range(nlanes):
        out += struct.pack("<I", 0x00010000 | (lane * 0x1357))
    for i in range(extra):
        out.append((i * 37 + 11) & 0xFF)
    return bytes(out)


def gen_streams():
    out = []
    for width, height, chroma, nsub in ((64, 64, 0, 3), (128, 64, 1, 3), (66, 66, 0, 0)):
        tx = (width + 63) // 64
        ty = (height + 63) // 64
        rows = [
            tile_row(0, ry, [tile(tx_i, nsub_log2=nsub, payload=rans_payload(1 << nsub))
                             for tx_i in range(tx)])
            for ry in range(ty)
        ]
        tools = 0x1 | (0x8 if chroma else 0)
        out.append(stream_header(width, height, chroma, tools) + frame(0, 24, rows))

    # A stream whose only frame is header-only: exercises the "no tiles" path.
    out.append(stream_header(64, 64) + frame(0, 0, []))
    # A TLV extension area with one private record the decoder must skip.
    ext = struct.pack("<HH", 0x8001, 4) + b"seed"
    out.append(stream_header(64, 64, ext=ext) + frame(0, 32, [tile_row(0, 0, [tile(0)])]))
    # Every tool bit the reference decoder supports, so the mask check passes
    # and the tool-dependent syntax is reachable.
    out.append(stream_header(64, 64, chroma=1, tools=0x3FF, alpha=1)
               + frame(0, 16, [tile_row(0, 0, [tile(0, payload=rans_payload(8))])]))
    # A transform-skip, res_level 2 tile: the smallest coding-unit geometry.
    out.append(stream_header(64, 64, tools=0x7)
               + frame(0, 0, [tile_row(0, 0, [tile(0, res_level=2, tskip=1,
                                                   payload=rans_payload(8))])]))
    return out


# ------------------------------------------------------------------ transport
def datagram(frame_id=0, tile_first=0, tiles=(), pose=False, band=0, caps=0x3F,
             path_seq=0, fec_k=0, fec_idx=0, flags=0, fec_m=0):
    h = bytearray(24)
    h[0] = 1 | ((flags & 0xF) << 4)
    h[1] = 0  # stream_id
    struct.pack_into("<H", h, 2, frame_id)
    struct.pack_into("<H", h, 4, tile_first)
    h[6] = len(tiles)
    # v2 (TRANSPORT.md 2, decision D19):
    #   byte 7 = layer_id [1:0] | frag_idx [3:2] | frag_count [5:4] | fec_class [7:6]
    #   byte 8 = band [2:0] | pose_hdr [3] | fec_m [6:4] | reserved [7]
    h[7] = 0
    h[8] = (band & 7) | (0x08 if pose else 0) | ((fec_m & 7) << 4)
    h[9] = caps
    struct.pack_into("<H", h, 10, 0)  # pose_seq
    struct.pack_into("<H", h, 12, path_seq)
    h[14] = 0
    h[15] = (fec_idx & 0xF) | ((fec_k & 0xF) << 4)
    struct.pack_into("<I", h, 16, 1000)
    struct.pack_into("<H", h, 22, 500)

    payload = b""
    if pose:
        payload += bytes(range(26))
    directory = b""
    body = b""
    for length, qp, mode in tiles:
        # len | qp | mode | res_level | lossless | chroma444 | alpha
        # | tile_class [27:26] | ref_delta [29:28]   (the last two are v2)
        e = (length & 0xFFF) | ((qp & 0x3F) << 12) | ((mode & 7) << 18) \
            | (0 << 26) | (3 << 28)
        directory += struct.pack("<I", e)
        body += bytes((i * 13 + 7) & 0xFF for i in range(length))
    payload += directory + body
    struct.pack_into("<H", h, 20, len(payload))
    return bytes(h) + payload


def gen_datagrams():
    out = []
    out.append(datagram(tiles=[(64, 24, 3)]))
    out.append(datagram(tiles=[(48, 20, 3), (0, 0, 0), (120, 30, 3)], tile_first=68))
    out.append(datagram(tiles=[(32, 16, 3)], pose=True, band=1, flags=0x1))
    out.append(datagram(tiles=[(200, 40, 3)], fec_k=10, fec_idx=0, path_seq=1, fec_m=3))
    # A parity datagram: tile_count == 0, payload is u16 L || block(L+2).
    h = bytearray(datagram(tiles=[])[:24])
    h[6] = 0
    h[15] = (10 & 0xF) | (10 << 4)
    h[8] = (h[8] & 0x0F) | (3 << 4)  # fec_m = 3
    L = 120
    par = struct.pack("<H", L + 2) + bytes((i * 7) & 0xFF for i in range(L + 2))
    struct.pack_into("<H", h, 20, len(par))
    out.append(bytes(h) + par)
    # A run that crosses a tile row: must be dropped, not placed.
    out.append(datagram(tiles=[(16, 24, 3)] * 4, tile_first=66))
    return out


def feedback(tiles_in_band=408, bands=1, mode=1, band_count=None):
    h = bytearray(8)
    h[0] = 1
    h[1] = 0
    struct.pack_into("<H", h, 2, 0)
    h[4] = 0
    h[5] = bands if band_count is None else band_count
    struct.pack_into("<H", h, 6, tiles_in_band)
    out = bytes(h)
    for b in range(bands):
        rec = bytearray(20)
        struct.pack_into("<H", rec, 0, 0)
        rec[2] = b
        rec[3] = mode | (1 << 2)  # complete
        struct.pack_into("<I", rec, 4, 1000)
        struct.pack_into("<I", rec, 8, 2000)
        struct.pack_into("<H", rec, 12, 800)
        struct.pack_into("<H", rec, 14, 0)
        struct.pack_into("<H", rec, 16, 0)
        bitmap = b""
        if mode == 0:
            bitmap = b"\xff" * ((tiles_in_band + 7) // 8)
        elif mode == 2:
            bitmap = bytes([2]) + struct.pack("<HB", 5, 3) + struct.pack("<HB", 100, 12)
        out += bytes(rec) + bitmap
    out += bytes([0, 0, 12, 20])  # trailer: loss, loss, rtt, rtt
    return out


def gen_feedback():
    return [
        feedback(mode=1, bands=1),
        feedback(mode=0, bands=1),
        feedback(mode=2, bands=3),
        feedback(mode=0, bands=3, tiles_in_band=272),
        feedback(mode=1, bands=3, tiles_in_band=1),
    ]


# ----------------------------------------------------------------------- rans
def gen_rans():
    out = []
    for nsub in (0, 3, 5):
        for tset in (0, 3):
            for flags, tables in ((0, b""), (1, bytes((i * 17 + 3) & 0xFF for i in range(120)))):
                nunits = 24
                prefix = bytes([nsub, tset, flags, nunits])
                out.append(prefix + tables + rans_payload(min(1 << nsub, nunits), extra=192))
    # A payload that is exactly the initial states and nothing else: the
    # decoder must refuse the first renormalization, not read past the end.
    out.append(bytes([3, 0, 0, 8]) + rans_payload(8, extra=0))
    # transform skip + the 4-coefficient DC-plane shape.
    out.append(bytes([3, 1, 0b0110, 8]) + rans_payload(8, extra=128))
    return out


# ------------------------------------------------------------------------ rs
def gen_rs():
    out = []
    for k, m, length, ed, ep, fl in (
        (10, 3, 1316, 0b0000000011, 0b000, 1),
        (10, 3, 1316, 0b0000000000, 0b000, 1),
        (4, 2, 64, 0b0011, 0b00, 1),
        (1, 1, 1, 0b1, 0b0, 1),
        (10, 4, 1360, 0b1111111111, 0b0000, 3),
        (7, 0, 512, 0b0000000, 0b0000, 0),
        (10, 3, 24, 0b0000000101, 0b010, 7),
    ):
        prefix = struct.pack("<BBHBBBB", k - 1, m, length - 1, ed & 0xFF, ep & 0xFF, fl, 0)
        body = bytes((i * 31 + 17) & 0xFF for i in range(256))
        out.append(prefix + body)
    return out


# ---------------------------------------------------------------------- warp
Q_NUM = 21
Q_DEN = 29


def warp_seed(h=None, ox=64, oy=64, tx=0, ty=0, mv=(0, 0), filt=0, mode=0,
              rw=64, rh=64, channels=1, depth=0):
    if h is None:
        h = [1 << Q_NUM, 0, 0, 0, 1 << Q_NUM, 0, 0, 0, 1 << Q_DEN]
    p = bytearray(56)
    for i, v in enumerate(h):
        struct.pack_into("<i", p, 4 * i, v)
    struct.pack_into("<hh", p, 36, ox, oy)
    struct.pack_into("<hh", p, 40, tx, ty)
    struct.pack_into("<hh", p, 44, mv[0], mv[1])
    p[48] = filt
    p[49] = mode
    p[50] = rw - 1
    p[51] = rh - 1
    p[52] = channels - 1
    p[53] = depth
    ref = bytes((i * 41 + 13) & 0xFF for i in range(2048))
    return bytes(p) + ref


def gen_warp():
    ident = [1 << Q_NUM, 0, 0, 0, 1 << Q_NUM, 0, 0, 0, 1 << Q_DEN]
    out = [
        warp_seed(),
        warp_seed(filt=1),
        warp_seed(mode=1, mv=(9, -13)),
        warp_seed(tx=64, ty=64, rw=192, rh=192, channels=3, filt=1),
        warp_seed(depth=1, rw=128, rh=128),
        # A slight rotation/zoom: h00 and h11 off unity, denominator still 1.0.
        warp_seed(h=[int(1.02 * (1 << Q_NUM)), int(0.03 * (1 << Q_NUM)), 5 << Q_NUM,
                     int(-0.03 * (1 << Q_NUM)), int(1.02 * (1 << Q_NUM)), -3 << Q_NUM,
                     0, 0, 1 << Q_DEN], filt=1, rw=192, rh=192),
        # A denominator row that is nonzero: the perspective path.
        warp_seed(h=ident[:6] + [1 << 12, -(1 << 12), 1 << Q_DEN], rw=128, rh=128),
        # Coordinates far outside the reference picture: the clamp path.
        warp_seed(tx=-1024, ty=30000, rw=8, rh=8),
    ]
    return out


# ----------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--clean", action="store_true",
                    help="remove existing seeds first (does not touch regressions/)")
    args = ap.parse_args()

    for t in TARGETS:
        d = os.path.join(args.out, t)
        os.makedirs(d, exist_ok=True)
        if args.clean:
            for f in os.listdir(d):
                os.remove(os.path.join(d, f))

    streams = gen_streams()
    for s in streams:
        write(os.path.join(args.out, "nxvc_decode_fuzz"), "gen-%s" % digest(s), s)
        write(os.path.join(args.out, "nxvc_headers_fuzz"), "gen-%s" % digest(s), s)

    # Real conformance vectors: valid all the way to the coefficients, which no
    # generator here can be.  Only the small ones are worth carrying in git.
    vecdir = os.path.join(args.repo, "tests", "vectors")
    copied = 0
    if os.path.isdir(vecdir):
        for name in sorted(os.listdir(vecdir)):
            if not name.endswith(".nxv"):
                continue
            path = os.path.join(vecdir, name)
            if name not in ALWAYS_VECTORS and (
                    os.path.getsize(path) > MAX_VECTOR_BYTES
                    or copied >= MAX_VECTORS):
                continue
            # ".nxv" is gitignored outside tests/vectors, so seeds carry a
            # neutral extension or they would never reach the repository.
            stem = "vec-" + name[:-4] + ".bin"
            shutil.copyfile(path, os.path.join(args.out, "nxvc_decode_fuzz", stem))
            # The header target only ever needs the headers; truncating keeps
            # the corpus small and keeps every input fast.
            with open(path, "rb") as f:
                head = f.read(256)
            write(os.path.join(args.out, "nxvc_headers_fuzz"), stem, head)
            copied += 1

    for blob in gen_rans():
        write(os.path.join(args.out, "nxvc_rans_fuzz"), "gen-%s" % digest(blob), blob)
    for blob in gen_datagrams():
        write(os.path.join(args.out, "transport_depacketize_fuzz"), "gen-%s" % digest(blob), blob)
    for blob in gen_rs():
        write(os.path.join(args.out, "transport_rs_fuzz"), "gen-%s" % digest(blob), blob)
    for blob in gen_feedback():
        write(os.path.join(args.out, "transport_feedback_fuzz"), "gen-%s" % digest(blob), blob)
    for blob in gen_warp():
        write(os.path.join(args.out, "warp_tile_fuzz"), "gen-%s" % digest(blob), blob)

    total = 0
    for t in TARGETS:
        d = os.path.join(args.out, t)
        n = len(os.listdir(d))
        size = sum(os.path.getsize(os.path.join(d, f)) for f in os.listdir(d))
        total += size
        print("%-30s %3d file(s)  %7d bytes" % (t, n, size))
    print("total %d bytes (%d conformance vectors copied)" % (total, copied))


if __name__ == "__main__":
    main()
