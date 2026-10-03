# Private Haar 4:2:0 motion fixture

Six independent native 4352x2176 4:2:0 frames derived from the existing dark stereo planar source. Y shifts are `(0,0),(2,2),(4,2),(6,0),(4,-2),(2,-2)` pixels; Cb/Cr shifts are half those values. `numpy.roll` wraps at borders intentionally. Each frame was encoded independently with the saved feature-correct host Haar 4:2:0 encoder at the existing 694328-byte target; the stream contains the common 40-byte header once, then six packet length+payload records.

Fixture: `motion-6.pyrowave` (4,165,684 bytes, SHA-256 `cd172d053d0b2ea63054fd919e697858825e341c7a2e6756b658796fec3c69e5`). Source: `frames/frame-00.yuv` SHA-256 `9c0dd82a1597e2c7b9a03ab5055456966e235b635bd7f2d31713f43eecf66516`. The encoder was an existing host binary SHA-256 `660db9b1456ff838de35c364a5173f367b9cfc8742e1129115e0baf95ff4bb94`.

The multi-frame helper was built against the current root host archive and current generated fused Haar shader. Run `./build-helper.sh`, then:

```sh
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./decode/root-haar-multiframe ./motion-6.pyrowave ./decode/out
```

The recorded run decoded all six frames; validation log contains no VUID/errors. Readback plane sizes per frame are 9,469,952 Y and 2,367,488 each Cb/Cr. Per-frame source MAE/max errors are in `decode/plane-errors.csv`. This is offline fixture validation only, not device timing, live FPS, jitter, or photon-to-photon evidence.
