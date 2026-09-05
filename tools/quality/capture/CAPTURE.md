# Recording a WiVRn session and running every gate on it

`corpus/README.md` is blunt about what the synthetic corpus is worth: it proves
the plumbing, and **the Phase 1 and Phase 2 gates are stated on VR captures**.
This page is the whole route from a headset on your face to
`tools/quality/reports/capture-<name>-<date>.md`, and it is three commands once
the server is built.

Everything here needs the WiVRn NX branch `nx-warp-capture`, which adds
`server/encoder/raw_dump.{h,cpp}`: a tap on the encoder's *input* that writes
the pixels the encoder saw plus the pose the frame was rendered for. It is off
unless `WIVRN_RAW_DUMP` names a directory, and it is not on any path a normal
session takes.

---

## 0. Build the capture server, once

```sh
cd /run/media/nerdrx/Lex/claude/wivrn-nx-capture     # the nx-warp-capture worktree
cmake -B build-server . -GNinja -DWIVRN_BUILD_CLIENT=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-server
```

And the reference codec, which the gates run:

```sh
cd /run/media/nerdrx/Lex/claude/nx-warp
cmake -S . -B build-ref -DCMAKE_BUILD_TYPE=Release && cmake --build build-ref -j2
```

## 1. Configure the session so the frames mean something

Three settings, and each one is the difference between test material and a
recording of an artefact.

**Encoder: anything but `vulkan`.** The tap copies out of the encoder's input
image, which it can only do while that image is in `eGeneral` on a queue family
the tap has a queue for. The Vulkan video encoders move it into a video-encode
layout on an encode queue; `raw_dump::create()` detects that and logs
`only eGeneral can be copied; not dumping` rather than producing something
wrong. `vaapi` is the right choice on this machine (`x264` also works and costs
a great deal of CPU). In `~/.config/wivrn/config.json`:

```json
{
	"bit-depth": 8,
	"encoder": { "encoder": "vaapi", "codec": "h265" }
}
```

10-bit works too — the tap writes P010 and `ingest_wivrn.py` down-converts with
dithering — but 8-bit is one fewer conversion between the compositor and a
number, so prefer it unless 10-bit is the thing being measured.

**Foveation off, on the headset.** The tap sits *after* the foveation
resample, so a foveated capture's pixels are not on a uniform angular grid —
and the pose homography of `docs/WARP.md` 4 assumes they are. `ingest_wivrn.py`
reads the foveation runs out of the `.jsonl` and **refuses** a capture whose
runs are not degenerate. Turn foveation off in the WiVRn client's video
settings before connecting.

**Motion smoothing and FSR1 off.** Motion smoothing synthesises frames, which
were never rendered from a pose and must not enter a codec test.

## 2. Record

```sh
mkdir -p $NXQ_SCRATCH/rawdump-turn
WIVRN_RAW_DUMP=$NXQ_SCRATCH/rawdump-turn \
WIVRN_RAW_DUMP_FRAMES=900 \
WIVRN_RAW_DUMP_MAX_MB=6000 \
  ./build-server/server/wivrn-server
```

`_FRAMES` is a per-stream cap and `_MAX_MB` a budget over every stream; both
are caps, not targets, and whichever is hit first ends the dump cleanly. The
defaults (300 frames, 8192 MB) give 3.3 seconds at 90 Hz, which is not enough
for the velocity split — set them.

**Raw frames are enormous.** At 1440x1440 NV12 one frame is 3.1 MB, so two eyes
at 90 Hz is **560 MB/s**:

| take | frames per stream | disk |
|---|---:|---:|
| 3.3 s (the default cap) | 300 | 1.9 GB |
| **10 s** | **900** | **5.6 GB** |
| 20 s | 1800 | 11.2 GB |

Put `$NXQ_SCRATCH` on the scratch volume, never on tmpfs.

### What to play, and for how long

Two takes, into two directories, because they answer two different questions
and neither substitutes for the other.

**Take 1 — 10 seconds of head rotation.** Any textured scene; stand still and
turn your head: rest for two seconds, a brisk turn across the scene, rest
again. That profile is not a preference, it is what the **Phase 2 kill test**
needs: PAPER 2.11 item 1 splits BD-rate at the fastest 20 % of frames, and a
take with no rest or no motion has nothing to split. `ingest_wivrn.py` prints
the angular-velocity range it found and warns when a take is too still.

**Take 2 — 10 seconds of a text-heavy application.** A menu, a browser panel, a
desktop window in VR; hold reasonably still. This is the 4:2:0 chroma-fringe
case PAPER 5.2 locks to lossless, and it is the content class where a codec
that looks fine on terrain falls apart.

Record them separately:

```sh
WIVRN_RAW_DUMP=$NXQ_SCRATCH/rawdump-turn WIVRN_RAW_DUMP_FRAMES=900 ... # take 1
WIVRN_RAW_DUMP=$NXQ_SCRATCH/rawdump-text WIVRN_RAW_DUMP_FRAMES=900 ... # take 2
```

Each directory ends up holding `stream0.yuv`, `stream0.jsonl`,
`stream0-info.json` and the same three for `stream1`.

## 3. Ingest

```sh
cd /run/media/nerdrx/Lex/claude/nx-warp
python3 tools/quality/capture/ingest_wivrn.py \
    --dump $NXQ_SCRATCH/rawdump-turn --name wivrn-turn-1440
```

This validates the info files, deinterleaves NV12 (or down-converts P010),
joins the two eye streams into one side-by-side coded picture in the layout
every corpus entry uses, converts the poses into a version 2 `.poses.json`
(`docs/WARP.md` 2.1), preserves the foveation runs as metadata, and registers
the sequence in `corpus/MANIFEST.json` under the `wivrn-capture` class with
SHA-256 hashes. It prints the sidecar path and the next command.

Files land in `$NXW_CORPUS` by default. Useful switches: `--pix
yuv420p,yuv444p` (444 doubles the size and only replicates chroma), `--frames
N`, `--layout mono --eye left`, `--dither none`, `--no-manifest`.

**Never commit the data.** These are recordings of your own session; the
manifest records their hashes so a result is reproducible *by the person who
has them*, which is the most a private capture can offer.

## 4. Run every gate

```sh
tools/quality/capture/run_gates.sh $NXW_CORPUS/wivrn-turn-1440.yuv420p.json
```

Phase 1 intra (PAPER 3.11), the Phase 2 kill test in both rate bands (PAPER
2.11 item 1), the warp-only chain (item 2) and the honest anchors — the same
four the synthetic material was measured on in
`tools/quality/reports/gates-v2-2026-09-04.md`, with the sequence swapped. It
writes `tools/quality/reports/capture-<name>-<date>.md` with every verdict
block quoted verbatim out of the logs, and leaves the result JSON and full
console output in `$NXQ_SCRATCH/results/capture-<name>/`.

It is hours. `--no-anchors` skips the long gate; `--frames N` shortens
everything, at the cost of a metric window narrower than the encode.

---

## The three commands

```sh
WIVRN_RAW_DUMP=$NXQ_SCRATCH/rawdump-turn WIVRN_RAW_DUMP_FRAMES=900 WIVRN_RAW_DUMP_MAX_MB=6000 ./build-server/server/wivrn-server
python3 tools/quality/capture/ingest_wivrn.py --dump $NXQ_SCRATCH/rawdump-turn --name wivrn-turn-1440
tools/quality/capture/run_gates.sh $NXW_CORPUS/wivrn-turn-1440.yuv420p.json
```

## Rehearsing it without a headset

Worth doing once before spending 5.6 GB and a session on it. `fake_raw_dump.py`
writes a directory in `raw_dump.cpp`'s exact format out of material the harness
already has, so the whole route can be driven in a minute:

```sh
python3 tools/quality/capture/gen_synthetic.py --out $NXQ_SCRATCH/seq \
    --name rehearsal --frames 6 --eye-width 128 --eye-height 128 --layout sbs --pix yuv420p
python3 tools/quality/capture/fake_raw_dump.py from-sequence \
    --seq $NXQ_SCRATCH/seq/rehearsal.yuv420p.json --out $NXQ_SCRATCH/fakedump
python3 tools/quality/capture/ingest_wivrn.py --dump $NXQ_SCRATCH/fakedump \
    --name wivrn-rehearsal-128 --no-manifest
tools/quality/capture/run_gates.sh --no-anchors $NXW_CORPUS/wivrn-rehearsal-128.yuv420p.json
```

The same generator drives `test_ingest_wivrn.py`, which asserts that the round
trip is lossless in the pixels and correct in the poses — including, through
`nxv-enc`, that the first warped frame of a pure-rotation pair clears 35 dB and
that conjugating every quaternion (the likeliest convention error, and one that
crashes nothing) costs more than 20 dB:

```sh
python3 -m pytest tools/quality/capture/test_ingest_wivrn.py -q
```

## Troubleshooting

| symptom | cause |
|---|---|
| no `stream*.yuv` at all | `WIVRN_RAW_DUMP` is unset, or the directory does not exist — the tap does not create it |
| `only eGeneral can be copied; not dumping` | the `vulkan` encoder; switch to `vaapi` or `x264` |
| `stream N is read on queue family K` | the encoder reads on a family the tap has no queue for; same fix |
| the dump stops early | `_FRAMES` or `_MAX_MB` reached; the log line says which |
| ingest: `this capture is FOVEATED` | foveation is on in the client; turn it off and re-record. `--allow-foveation` converts it anyway and marks it as not gate material |
| ingest: `frame_bytes ... disagree` | a dump interrupted mid-write, or the wrong directory |
| a warning about frustum asymmetry | expected on a real headset: `nxv-enc` builds a centred `K` from `fov_deg` and a real `XrFovf` is not symmetric. The measured half-angles are kept in `fov_rad` and `capture.fov`; the residual is a projection error the codec is charged for |
