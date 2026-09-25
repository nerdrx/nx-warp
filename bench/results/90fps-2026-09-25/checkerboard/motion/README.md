# Synthetic checkerboard motion sample

`synthetic-motion-90fps.mp4` shows a generated sharp edge and text moving
through checkerboard reconstruction at 90 fps. `synthetic-motion-10x-slow-90fps.mp4`
repeats each source frame ten times to make the temporal pattern easier to see.
Source images are generated from scratch; no private photos or headset captures
are included.

The 90 files inside `synthetic-nxdf.tar.gz` are Vulkan fixture output. The archived
`testsrc/direct_checkerboard_motion.cpp` applies the real checkerboard wire
transform and renders a CPU equivalent of the client's selection rules. Its
four panels show full decode, valid previous-frame reuse, nearest-current
fallback, and current-phase samples. Mode-2 peripheral cells span 4×4 output
pixels. This is CPU reconstruction, not device decoding, Vulkan presentation,
WiVRn transport, or a Pico image-quality result.

To replay the exact recorded synthetic inputs, start in this report directory:

```sh
tar -xzf motion/synthetic-nxdf.tar.gz -C motion
mkdir -p motion/frames
c++ -std=c++20 -O2 -I testsrc testsrc/direct_checkerboard_motion.cpp -o /tmp/direct-checkerboard-motion
/tmp/direct-checkerboard-motion motion/frames/frame- motion/encoded/frame-[0-9][0-9][0-9].nxdf
python3 motion/compose_video.py
```

Pillow, DejaVu fonts and FFmpeg are required for video composition. These
commands reproduce the normal-speed MP4. The 10x version repeats each frame ten
times and includes an explicit slow-motion banner.

To generate new source/encoded inputs, `generate_synthetic_inputs.py --fixture
/path/to/direct_blocks_gpu_fixture` takes a built WiVRn fixture executable.
`testsrc/direct_blocks_gpu_fixture.cpp` preserves its source; building that GPU
fixture requires the matching WiVRn encoder backend and Vulkan dependencies.
Replaying the shipped archive does not require Vulkan or private source photos.

The included `source/*.png` are selected generated stills. `motion-preview.png`
and `slow-preview.png` are titled sample frames.
