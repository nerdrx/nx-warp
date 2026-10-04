# ASTC CEM6 RGB-scale probe

Offline PC-only experiment. Scratch shader compares projected-line one-partition q6 against fixed three-seed two-partition RGB-scale blocks. No production shader, client, Pico, allocator, or hardware runtime was changed.

## Verified endpoint budget

Basis’s primary decoder calls RGB-scale endpoint mode **6**; literal mode 4 decodes luminance-alpha. For mode 0x053, a two-partition shared-CEM block reserves 29 configuration bits. A 4×4 grid of 3-bit weights takes 48 bits, leaving 51 endpoint bits. CEM6 supplies four endpoint values per partition, eight total. Basis `computeMaximumRangeISEParams(51, 8)` selects quint4: `ceil(8×7/3) + 8×4 = 51` bits, with 80 legal codes per endpoint value. The shader uses Basis’s quint encode mapping and matching unquantization. Endpoint payload packs 19 + 19 + 13 bits; all emitted full-frame ASTC files independently decode successfully.

## Result

Exact source paths and SHA256 values are in `results/manifest.json`; quality and cost comparison is in `results/comparison.csv`. Dark changes 30.2679→30.2848 dB PSNR; forest 36.6826→36.6856 dB. Zstd level-3 grows by 288 and 12 bytes. GPU encode median rises 0.016→0.430 ms and 0.027→0.362 ms. Only 96 dark blocks and 4 forest blocks select CEM6. This is not a material tradeoff, so the mode is rejected.

The native crowd original remains unresolved; no crowd quality claim is made. The old crop was not used for this comparison.
