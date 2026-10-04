# ASTC endpoint refit probe

Isolated PC-only Vulkan experiment in a new scratch directory. It starts from the projected-line q6 encoder and holds the fitted 5×5 three-bit weight symbols fixed. It interpolates their exact ASTC dequantized values at all 64 texel centers, solves the 2×2 least-squares endpoint system, requantizes endpoints to the existing six-bit precision, and repacks the same legal ASTC mode. If endpoint order reverses, it swaps endpoints and complements the 3-bit symbols.

`results/manifest.json` records source paths and SHA256 hashes matching the provenance-checked native dark and forest inputs. `results/comparison.csv` compares independent host decode RGB MSE/PSNR/MAE, full-file Zstd level-3 bytes, and resident GPU encode time for the projected-line baseline and refit. All tested ASTC files decoded successfully. The shader compiled with glslang and passed `spirv-val`.

The result is small and mixed: dark gains 0.070 dB and 164 Zstd bytes; forest gains 0.116 dB but MAE rises slightly and Zstd grows by 485 bytes. GPU median rises about 0.029–0.040 ms per 1920×1080 image. This was not judged material enough for q2 or temporal testing. No client, allocator, production shader, or Pico path was touched.
