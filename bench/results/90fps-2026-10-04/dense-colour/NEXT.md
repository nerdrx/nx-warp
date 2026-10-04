# Current handoff — 2026-10-04

Deadline: **07:00 UTC / 09:00 Europe/Berlin**. Keep existing heartbeat active until then; pause at deadline.

- WiVRn checkout: `/run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe`, branch `pyrowave-probe`. PC colour fit pushed `668eea70`; submitted-GPU-only JIT calibration pushed `71ee1246`. Preserve unrelated NXWarp README/branding dirt.
- Report repo: `/run/media/nerdrx/Lex/claude/nx-warp`, main. Colour report pushed `cbc7d47`; JIT report awaits owned-path commit.
- Installed Pico APK SHA `430ae866e7820783b5bd5382cd123864b6b484ad99a421426764c9e94f191e8b`, local path `nx-scratch/astc-jit-gpu-warmup-20261004/packaging/presentation-jit-release.apk`. Existing app data and signature preserved.
- Current profile: ASTC 8×8, native 2176×2176 per eye, 90 Hz, no foveation/JPEG/motion-field warp/blur, requested `--no-encrypt`. Neutral presentation shader. Existing WayVR only, no demo or mouse interaction.
- Latest short stationary wake capture: four 2 s windows at 89.3–90.3 render iterations/s, 177–180 fresh frames, 2.3–3.0 ms own GPU pass; 12 non-black encoder means, two idle-black means. No sustained motion, complex-scene or photon-latency proof.
- Smaller blocks, RGB-scale, endpoint refit, whole-frame partition search, compacted one-seed partition fit measured and rejected. Exact two raw user photos verified; original crowd clipboard missing. Legacy crowd fixtures have unverified preprocessing and cannot prove native detail.
- Active Luna scratch probes: `astc-partition-coarse-grid-20261004` high-precision palette ceiling; dual-plane quality ceiling. No production changes authorized by results yet. Root also tests newest complete ASTC stereo-pair selection: current logs often select an older frame despite newer ready pair. Preserve explicit de-jitter and exact stereo matching.
- Runtime helpers: `/run/media/nerdrx/Lex/claude/nx-scratch/live/nxastc-20261003/{start-server.sh,restart-clean.py,reconnect-client.py}`. Verify owned PIDs before stopping; wake bursts only sustain off-head XR briefly. Do not call idle black 90 FPS content proof.
- Server build uses pinned Vulkan headers `tools/local/include` 1.4.309, SPIR-V Vulkan 1.3. Android native build `build/android-arm64`, Release NDK29 arm64 API29. Existing signing key must be reused; never uninstall on signature conflict.

Continue bounded quality and latency work until deadline. Publish only owned, validated changes and original evidence. Leave the best verified build ready.
