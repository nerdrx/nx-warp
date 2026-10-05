# Compositor encoder-work lifecycle gate

This is a CPU-only, source-extracted branch check for the ASTC eye-work path. It executes the exact source block in `server/compositor/compositor.cpp` from the encoder snapshot (`auto encoders = get_encoders();`) through `image.busy = false;`; the extracted-block hash and line span are in `raw/luna/provenance.txt`. It also reads `num_streams` and `quad_stream_idx` from `server/encoder/encoder_settings.h` (4 and 3 at this source revision). Only the fakes and surrounding harness are authored here; the compositor source is unchanged.

The fakes model the base encoder interface, ASTC dynamic type, alpha stream 2, promoted quad stream 3, session, view info, encoder snapshot and atomic image-busy bit. Latches demonstrate overlap and hold the right eye while checking the image remains busy; encoder replacement then verifies the snapshot's old right-eye object stays alive until the work completes. Other cases check option absent/`0`/non-exact values, hardware, missing eye, alpha/quad fallback, both per-eye exception paths, and serial fallback after a one-shot injected async-launch failure.

The launch-failure build differs only at the `std::async` call expression: a one-shot wrapper throws there, then delegates normally thereafter. This checks the source catch/fallback branch; it does not emulate platform thread-launch internals. The harness is not the full compositor and does not exercise Vulkan, real encoders, GPU work, scheduler performance, or performance.

## Run

```sh
rtk proxy ./run-check.sh /run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe raw/luna
```

The wrapper bounds each executable to 30 seconds. Normal and halt-on-error ASan/UBSan runs pass: 60 checks on the unmodified branch and 64 with injected launch failure. Captured tagged outcome rows are in `raw/luna/{normal,launch-failure}.log`; source, extracted-block and fake-launch hashes are in `hashes.txt`. No TSan run was performed; this does not establish race freedom.
