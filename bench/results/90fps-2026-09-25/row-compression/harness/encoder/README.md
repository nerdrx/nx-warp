# Native row-predictor ABBA harness

This harness compares the `08f55c16` codec snapshot with the current row-capable
codec through the production offscreen Vulkan fixture. It exercises no-ACK
independent frames, acknowledged forest/dark pan pairs, a forest-to-dark scene
cut, and both global and four-region motion settings. Each timed sample is a
repeated two-frame pair: reset references, encode one anchor, then encode one
shifted current frame. It is not a continuous-motion sequence.

`crop_native.py SOURCE OUTPUT --shift N` crops the centre 256x256 RGBA pixels
from a raw 2160x2160 RGBA photo. It duplicates the crop for both eyes and shifts
the source x-coordinate by `N` before the codec's native-centre foveation step.
Pass source paths at run time; no source photo is copied into this harness. The
offscreen Vulkan periphery stays the fixture's synthetic solid-color image,
while the native-centre crop comes from the photo. Generated crop buffers are
derived test data and should stay local.

Every control and measured frame is restored and byte-compared with a separate
independent encode of the same current input. The CSV records the restored
FNV-1a hash and oracle result as well as whole-stream/detail sizes, selected
mode, nested Zstd body version, and encode time.

`build.py --link` builds only standalone codec objects and test binaries using
the existing `motion-live-server-build` compile/link commands. It does not
build or launch the server. `run_abba.py` runs baseline, candidate, candidate,
baseline; each process has six warmup pairs followed by 24 timed encode pairs.
It runs global and regional motion modes unless narrowed with `--regions`.
The baseline/current outputs can be compared for wire-size change and `FRAME`
mode counts identify row-v3, global motion, regional motion, and independent
output.

The baseline source was extracted from commit `08f55c16af6e96980d5b5f522b70efc6ca8492dd`;
SHA-256 `3ed8ce30d60e9373ac68939e295f10335c650e46c6b6a7ca64e2725e361d385c`.
The published run completed with exact reconstruction and no matched size regression. Use an existing server build and pass both `--repo` and `--build` explicitly. Private source images and derived buffers are not included.

Reproduce using caller-supplied raw photographs and an existing WiVRn NX build:

```sh
python3 crop_native.py "$FOREST_RGBA" forest-s0.native.rgba --shift 0
python3 crop_native.py "$FOREST_RGBA" forest-s8.native.rgba --shift 8
python3 crop_native.py "$DETAIL_RGBA" dark-s0.native.rgba --shift 0
python3 crop_native.py "$DETAIL_RGBA" dark-s8.native.rgba --shift 8
python3 build.py --repo "$WIVRN_REPO" --build "$WIVRN_BUILD" --link
python3 run_abba.py --fixture-dir . --output encoder-samples.csv
```
