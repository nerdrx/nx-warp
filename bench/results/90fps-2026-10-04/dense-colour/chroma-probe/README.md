# Synthetic 4:2:0 chroma sampling probe

Offline PC-only comparison. This models one shader detail from the PyroWave
probe: per-pixel luma with 2×2-shared chroma. It does not run or reproduce the
live GPU YCbCr sampler path exactly.

The probe converts each fixture to full-range BT.709. It keeps each luma sample
at native resolution, averages Cb/Cr over each 2×2 luma footprint, then
reconstructs RGB with either nearest chroma or bilinear chroma. Bilinear uses
centred chroma sample positions: `(luma_pixel_center - chroma_footprint_center)`
mapped to the 2×2 chroma grid, with edge clamping. Original RGB, nearest RGB,
and bilinear RGB each pass through the same Vulkan ASTC 8×8 harness, fit 3, at
q6 and q3. ASTC block payloads are compressed with Zstd level 3 and round-trip
verified. `source_reconstruction_psnr_db` measures synthesized RGB vs fixture;
`decoded_vs_original_psnr_db` measures decoded ASTC vs fixture.

| Scene | Q | Original RGB Zstd3 | Nearest Zstd3 | Bilinear Zstd3 | Nearest PSNR vs original | Bilinear PSNR vs original | Original RGB PSNR |
|---|---:|---:|---:|---:|---:|---:|---:|
| Dark | 6 | 416,380 B | 416,454 B | 406,538 B | 31.744 dB | 31.618 dB | 31.741 dB |
| Forest | 6 | 258,372 B | 242,345 B | 228,052 B | 40.047 dB | 39.854 dB | 40.042 dB |
| Crowd | 6 | 116,060 B | 116,021 B | 169,506 B | 28.191 dB | 27.825 dB | 28.181 dB |
| Dark | 3 | 288,041 B | 288,048 B | 280,039 B | 29.787 dB | 29.711 dB | 29.784 dB |
| Forest | 3 | 184,735 B | 170,731 B | 159,985 B | 33.206 dB | 33.159 dB | 33.202 dB |
| Crowd | 3 | 100,891 B | 100,827 B | 125,976 B | 27.008 dB | 26.685 dB | 26.997 dB |

Nearest preserves the source most closely on dense-colour crowd imagery. The
bilinear model does soften 2×2 chroma transitions while leaving the luma plane
unfiltered, but it lowers decoded crowd PSNR and raises Zstd3 payload by 46% at
q6 and 25% at q3 versus nearest. Forest payloads shrink with bilinear filtering,
with small decoded-PSNR loss. This scene dependence does not support a general
quality win or a Pico implementation decision by itself.

The 1:1 crop shows source, pre-encode reconstructions, and q6 ASTC outputs at a
dense coloured edge. The q6/q3 measurements and all scene records are in
`results/source-chroma.csv`. `compare_source_chroma.py` reproduces the run using
the adjacent existing fixtures, Vulkan harness, decoder, and system `zstd`.

Luma is copied unchanged into the synthetic YCbCr reconstruction before 8-bit
RGB quantization. ASTC still changes luma as part of normal RGB block encoding.
This probe measures no shader instruction cost, sampler latency, Pico power,
headset output, or live stream performance. Bilinear texture sampling may add
Pico work; this PC-only RGB harness cannot measure that cost. ASTC's 8×8 palette
capacity also remains unchanged.
