# Borrowed-R8 catchup fixture

`test_borrowed_r8_catchup.cpp` is an explicit regression target for the
three-target, eight-frame dirty-history sequence. It requires an NXV fixture
and is intentionally not registered as a default CTest: ordinary test runs do
not depend on generated files.

The metrics run used the small 128×128-per-eye, eight-frame stereo YUV fixture at
`/tmp/atlas8.yuv`, encoded as `/tmp/atlas8.nxv` with the repository's
`nxvc-vkenc` tool. Recreate a deterministic changing-pixel YUV input with:

```sh
python3 tests/vk-decoder/atlas/generate_atlas8_fixture.py /tmp/atlas8.yuv
nxvc-vkenc --in /tmp/atlas8.yuv --w 256 --h 128 --pix yuv420p --qp 26 --frames 8 --eyes 2 --atlas --atlas-mode --inter --out /tmp/atlas8.nxv
```

Run the explicit target as
`nxvc-atlas-borrowed-r8-catchup-test /tmp/atlas8.nxv`. The test compares every
frame against decoder-owned pixels while cycling targets 0→1→2, requires frame
3 to remain ATLAS, then clears the borrowed target and checks owned restoration.
It also requires changing owned pixels, so an all-static fixture cannot satisfy
the partial-reuse assertion. Decoder stats provide the frame-mode diagnostic;
the test does not claim performance.
