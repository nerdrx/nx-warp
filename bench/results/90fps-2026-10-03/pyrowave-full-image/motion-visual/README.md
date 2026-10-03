# Paired-Haar motion-crop visual check

This is an offline crop comparison from six independently encoded full-size
4:4:4 frames. The preview shows source / CDF 9/7 / paired Haar crops in each
frame. The source image and full-size decoded planes stay in private scratch;
only the 512-pixel crops, scripts, and plane-error table are in this folder.

The six frames are small translations of the same native 4352x2176 stereo
image: (0,0), (1,0), (2,1), (1,2), (0,1), (-1,0) pixels, edge-clamped. Each
frame was encoded as a standalone image at the same 694,328-byte target. This
is a spatial-quality spot check; it does not test inter-frame coding, temporal
stability/jitter, playback, or headset quality.

## Reproduce

Private inputs and binaries are in `/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-haar-20261003` and `/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-matched444-20261003`.

```sh
cd /run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-haar-20261003
python3 motion-visual/make-sequence.py
for i in 00 01 02 03 04 05; do
  ./pyrowave-haar-host-decoder motion-visual/haar-frame-$i.pyrowave motion-visual/out-haar-$i
  /run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-matched444-20261003/pyrowave-host-fragment-readback motion-visual/cdf-frame-$i.pyrowave motion-visual/out-cdf-$i
done
python3 motion-visual/create-preview.py
```

The host Haar encoder/compute decoder and CDF fragment decoder used their
feature-enabled Vulkan logical devices. The preview reconstructs RGB from
planar full-range BT.709 4:4:4 planes, then crops x=1350..1861, y=1320..1831
from the duplicated stereo frame. Both decoded methods are compared to the
exact translated source Y/Cb/Cr planes. Encoder executables: Haar SHA-256
`6bbb6fe513095bd48dd69beaff9172f9c6652afad59e3d94305ebb4f098740de`; CDF
`9f11b5a3942c13417a838b6577823904dd91a98a8538f9e28e89f6a6fe77a36e`. Feature-
enabled CDF decoder SHA-256 `30a977cc0c3da53f2af374a471ab2e333a7b7703284a173a730d2ba91c265cdf`.

## Result

All 12 encodes and actual host decodes completed. Each fixture is 694,268 to
694,356 bytes including its 44-byte header. Across six frames, mean full-plane
absolute errors were:

| Decoder | Y MAE | Cb MAE | Cr MAE |
|---|---:|---:|---:|
| CDF 9/7 | 1.3402 | 1.1737 | 1.0770 |
| Paired Haar | 1.6383 | 1.3637 | 1.2153 |

Paired Haar is modestly less accurate by these metrics. The crop is intended to
make dark texture and edges easy to inspect; it is a selected region, not a
whole-frame image-quality score. `plane-mae.csv` contains per-frame values.
