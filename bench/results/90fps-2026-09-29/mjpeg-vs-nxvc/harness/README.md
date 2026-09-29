# Reproducing the JPEG/NXVC comparison

Requires a C++20 compiler, the WiVRn NX `common/` and `server/encoder/` headers,
installed LZ4, Zstd and libjpeg-turbo 3 development libraries, Python with Pillow,
NumPy and Matplotlib, and the six caller-supplied raw NXDF fixtures listed in
`../fixtures.json`. Photos and generated JPEG/PPM/container data must stay outside
the public results folder. The tested production revision and header hashes are
in `../host-method.json`.

Set `WIVRN`, `CORPUS`, `PRIVATE` and `RESULTS` to local paths. `RESULTS` is this
report directory, and `PRIVATE` is a separate writable scratch directory.

```sh
c++ -O3 -DNDEBUG -std=c++20 -I "$WIVRN/common" -I "$WIVRN/server/encoder" \
  export_nxdf.cpp -o "$PRIVATE/export_nxdf"
c++ -O3 -DNDEBUG -std=c++20 -I "$WIVRN/common" \
  nx_independent_bench.cpp -lzstd -llz4 -o "$PRIVATE/nx_bench"
c++ -O3 -DNDEBUG -std=c++17 jpeg_decode_bench.cpp -lturbojpeg \
  -o "$PRIVATE/jpeg_decode_bench"

"$PRIVATE/export_nxdf" "$CORPUS" "$PRIVATE/decoded"
"$PRIVATE/nx_bench" "$CORPUS" "$PRIVATE/nx-output" "$RESULTS/nx-summary.json"
python3 compare.py --corpus "$CORPUS" --decoded "$PRIVATE/decoded" \
  --private "$PRIVATE/encoded" --results "$RESULTS"
python3 compare.py --corpus "$CORPUS" --decoded "$PRIVATE/decoded" \
  --private "$PRIVATE/aligned444" --results "$RESULTS/aligned444" \
  --atlas-guard 0 --chroma 444 --arm sample-atlas
```

The first comparison emits 168 rows (6 inputs × 7 qualities × 2 chroma modes ×
2 representations). The aligned-atlas arm emits another 42 rows. The comparison
script asserts lossless sample-atlas reconstruction before any JPEG encoding.
`decode_packet()` can independently read the generated `.mjxa` containers.
Container version 1 uses four-pixel guards and 16-pixel slot alignment; version 2
uses no guards and eight-pixel alignment, for 4:4:4 only.

The exporter follows the production row-interleaved stereo descriptor layout.
It uses `native_base_pixel()` for peripheral palette reconstruction, direct native
RGB888 reads, and rejects unexpected native tile locations or non-duplicated
eyes in this specific saved corpus. These assertions are fixture constraints,
not claims about a general stereo decoder.

For timing one JPEG, run `jpeg_decode_bench input.jpg samples.csv`. For an atlas,
pass every atlas JPEG before the final CSV argument: both images are decoded
within each measured interval. The private `decode-jobs.json` files enumerate
the exact JPEGs. CPU timing excludes metadata restoration, reassembly, output
upload, safety decoding and presentation. `exact` in the JPEG raw timing CSV
means stable output across repetitions, **not lossless JPEG compression**.

NX helper timing includes helper allocation, parsing and reconstruction into
NXDF bytes. It uses six warmups and 24 measured repetitions. It is deliberately
not presented as equivalent work to JPEG-to-RGB decode.

The Python metrics use Pillow's libjpeg-turbo 3.2.0; the native timing harness
uses system libjpeg-turbo 3.2.0. JPEG encoding uses optimized Huffman tables.
No rate controller, streaming session, desktop window or GPU workload is started.
