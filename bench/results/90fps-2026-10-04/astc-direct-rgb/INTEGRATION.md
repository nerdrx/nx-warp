# Integrated source

Direct RGB input is committed as `9d05f4da` on the custom WiVRn NX `pyrowave-probe` branch. It remains opt-in through `WIVRN_ASTC_DIRECT_RGB=1`; the default retains planar input. The final guard also requires identical left/right extents.

The earlier equivalence and native smoke measurements in this report used `62eff3af` plus the direct-RGB working-tree patch. Committing that patch does not turn those measurements into a new binary test. The included zero-context `source-diff.patch` reproduces the seven server-file changes from `62eff3af` to `9d05f4da`; apply it with `git apply --unidiff-zero`. Shader math is unchanged by the final guard correction.

[Source documentation](https://github.com/nerdrx/wivrn-nx/blob/pyrowave-probe/docs/ASTC_DIRECT_RGB.md) · [Photo proxy comparison](../astc-rgb-input-quality/README.md). No inside-headset image inspection or controlled live bitrate comparison was performed.
