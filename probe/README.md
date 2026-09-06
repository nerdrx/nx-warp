# `probe/` — the texture-native atlas measurement harness

Standalone, headless, arm64. Pushed to `/data/local/tmp/nxtex` and run over adb.
It never installs a package, never starts a session, and never touches
`wivrn-server` or the installed `org.meumeu.wivrn.nx.warp` app.

    ./build.sh                                   # build + push
    adb shell /data/local/tmp/nxtex/nxtexnative --formats
    adb shell /data/local/tmp/nxtex/nxtexnative --bench   [--format F] [--payload F] [--nearest]
    adb shell /data/local/tmp/nxtex/nxtexnative --update  [--ntiles N] [--strips]

`--formats` answers the gate question: does the Adreno 650 sample this
compressed format with `SAMPLED_IMAGE_FILTER_LINEAR`.

`--bench` runs ADR-0029 section 5's display pass — one warp step from the atlas,
per-tile homography, bilinear by the sampler — over a 2176x1088 atlas writing
2176x1088 of output, which is 1088x1088 per eye, one displayed frame pair. The
shader is byte-identical between runs; only the atlas `VkFormat` changes.

`--update` measures `vkCmdCopyBufferToImage` of N refreshed 64x64 tiles into the
atlas, which is the only way to write a compressed image (none of these formats
carries `STORAGE_IMAGE`). `--strips` re-expresses the same bytes as full-width
64-row regions, which isolates per-region overhead from bandwidth.

Results are in `results/`. See `docs/TEXTURE-NATIVE-PATCHES.md`.
