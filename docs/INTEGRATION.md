# NX Warp in WiVRn NX: integration plan

Status: spike, written against `wivrn-nx` @ `nx-patches` `79a65389` and `wivrn-nx-windows` @ `e8e4806`.
Nothing in this document has been built into either tree except the capture patch of the last
section, which lives on `wivrn-nx-capture`.

Every path below is real and was read. Line estimates are counted against the files they would
touch, and are for a working Phase 3 integration, not a prototype.

Two corrections to the assumptions the paper carries about the host project, both of which change
work:

1. **The server's encoders do not live under `server/driver`. They live under `server/encoder`.**
   `server/driver` is the Monado device driver (poses, trackers, session, bitrate controller).
2. **The image handed to an encoder is not the compositor's RGBA render target.** It is already
   YCbCr 4:2:0, already foveated, and already squashed to one image per eye layer. Section 3.6 of
   the paper assumes the encoder gets linear RGB and does its own colour conversion. It does not.
   This is the single largest piece of unplanned work on the server side; see 1.3.

---

## 1. Server

### 1.1 Where `video_encoder_nxwarp` plugs in

The seam is exactly one abstract class:

```cpp
// server/encoder/video_encoder.h
class video_encoder
{
protected:
    virtual void present_image(vk::Image y_cbcr,
                               vk::SemaphoreSubmitInfo sem_info,
                               uint8_t slot,
                               uint64_t frame_index) = 0;
    virtual std::optional<data> encode(uint8_t slot, uint64_t frame_index) = 0;
};
```

with `struct data { video_encoder * encoder; std::span<uint8_t> span; std::shared_ptr<void> mem;
bool prefer_control; }`.

New files:

| File | Lines | Content |
|---|---|---|
| `server/encoder/video_encoder_nxwarp.h` | ~120 | class, per-slot state, descriptor arrays |
| `server/encoder/video_encoder_nxwarp.cpp` | ~700 | E0–E5 submits, rate-control feedback, segment table |
| `server/encoder/nxwarp_idr_handler.{h,cpp}` | ~120 | per-tile reference tracking; replaces the IDR ladder |

`server/encoder/video_encoder_raw.{h,cpp}` (47 + 190 lines) is the model to copy: it is the only
existing encoder that is not a wrapper around a vendor SDK, and it already does the
present-side `copyImageToBuffer` / encode-side fence wait split that the codec needs, with the
same two-slot cadence.

Registration is three edits in `server/encoder/video_encoder.cpp`
(`video_encoder::create`, around line 230):

```cpp
inline const char * encoder_nxwarp = "nxwarp";      // video_encoder.h, next to encoder_raw
...
if (settings.encoder_name == encoder_nxwarp)
    res = std::make_unique<video_encoder_nxwarp>(wivrn_vk, settings, stream_idx);
```

plus one line in `server/CMakeLists.txt` (the `target_sources` block at line 115) and a
`WIVRN_USE_NXWARP` option next to `WIVRN_USE_X264`.

`video_encoder::create` also has a `switch (settings.codec)` per encoder backend that must gain
the new enumerator or it will not compile — the switches are exhaustive and unguarded.

### 1.2 Device, queue, image, layout, sync

All four come for free; this is the part of the paper that is already true.

* **Device.** `wivrn::vk_bundle` (`server/utils/wivrn_vk_bundle.h`) holds
  `vk::raii::Instance/PhysicalDevice/Device`, a VMA allocator, and
  `queue_data { queue_mutex mutex; vk::raii::Queue queue; uint32_t family_index; }` for
  `queue` (graphics/compute, shared with Monado's compositor through
  `queue_mutex::share(pthread_mutex_t&)`), `transfer_queue`, and up to three `encode_queues`.
  It is Monado's own `VkDevice`, so there is no external memory anywhere. `api_version` is 1.3.
  The encoder should take `vk.queue` (compute) — **not** an `encode_queue`, which is a
  video-encode-only family with no compute capability.

* **Image.** `server/compositor/compositor.cpp:205` `make_images()`. One `VkImage` per
  double-buffer slot, `arrayLayers = 3` (left, right, alpha), format
  `eG8B8R82Plane420Unorm` at 8 bit or `eG10X6B10X6R10X62Plane420Unorm3Pack16` at 10 bit, created
  `eMutableFormat | eExtendedUsage` with an `ImageFormatListCreateInfo` of
  `{R8, R8G8, G8B8R8_2PLANE_420}` so the compositor writes the planes through single-plane
  storage views. `usage = eStorage | eTransferSrc` (plus `eVideoEncodeSrcKHR` when a Vulkan
  video encoder is present). The encoder reads `src_layer` (`encoder_settings::src_layer`,
  0/1/2), which the base class exposes as `const uint32_t src_layer`.

  **The codec needs a fourth view format in that list.** Pass B/E3 want to read and write the
  luma and chroma planes as `r8ui`/`rg8ui` (or `r16ui`/`rg16ui` at 10 bit) storage images.
  `eR8Unorm`/`eR8G8Unorm` are already in the list; unorm storage images are fine for the codec's
  integer path only if the shader converts, so add the `Uint` variants to
  `image_formats()` and the `pViewFormats` array — 4 lines, and free, because
  `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` is already set.

* **Layout and ownership.** `compositor.cpp:925-960` records, per encoder, one
  `vk::ImageMemoryBarrier2` per frame: `eGeneral → encoder->target_layout`,
  `srcQueueFamilyIndex = vk.queue.family_index`, `dstQueueFamilyIndex = encoder->target_queue`,
  `subresourceRange.baseArrayLayer = encoder->src_layer`. `target_layout` is a public mutable
  member defaulting to `eGeneral`, and the header says it "must not change after the first
  frame". For NX Warp it stays `eGeneral` (storage read) and `target_queue` is
  `vk.queue.family_index`, which makes the barrier a pure layout no-op and there is no
  ownership transfer at all. Nothing in the compositor changes.

* **Sync.** The compositor signals one timeline semaphore per submission and hands the value in:
  `present_image(vk::Image, vk::SemaphoreSubmitInfo{semaphore, value, stageMask}, slot,
  frame_index)`. The encoder overwrites `stageMask` with its own (the raw encoder uses
  `eTransfer`; NX Warp uses `eComputeShader`) and puts it in `pWaitSemaphoreInfos` of its
  `submit2`. This is exactly the "compositor signals value F, encode waits on it" model of
  paper 3.6. Row-group pipelining (timeline value `8F + g`) is the encoder's own second
  timeline and is invisible to the compositor.

  One constraint the paper does not mention: `vk.queue.mutex` is *Monado's* queue mutex. Every
  `vkQueueSubmit` on the compute queue serialises against the compositor's own submits. Eight
  small submits per frame per stream (two eyes = 16) is 16 mutex acquisitions inside the
  compositor's critical path. Either batch the row groups into fewer submits, or take a
  dedicated compute queue at device creation in `wivrn_vk_bundle.cpp` (about 30 lines, and the
  cleaner answer).

### 1.3 The colour-space problem

`server/compositor/layer_squasher.cpp` + `foveation.cpp` composite the application layers,
resample them through the foveation curve, and write **YCbCr 4:2:0** into the two-plane image, in
one compute pass, before any encoder sees anything. So:

* the codec's E0 warp and E3 reconstruct would run on 4:2:0 chroma, not on the RGB the
  bit-exactness rules and the reprojection hand-off assume;
* paper 6.8's "the client always holds a full-resolution reference" is fine, but the reference is
  then YCbCr and the decoder's output image must be YCbCr too, or the client pays a conversion;
* per-tile resolution levels (paper 1.5, 5.1) interact badly with a chroma plane that is already
  half resolution.

Three options, in increasing order of work and correctness:

1. **Take the 4:2:0 image as-is.** Y at full tile resolution, CbCr as a half-resolution
   companion plane per tile. Zero compositor change. Costs the 4:4:4 fovea of Phase 4 and makes
   the "one bit-exact integer pipeline" claim awkward at plane boundaries. ~0 lines server-side,
   and the client's reprojection keeps its existing `VkSamplerYcbcrConversion` path.
2. **Ask the compositor for an RGB target when the codec is active.** `image_formats()` gains an
   `eR8G8B8A8Unorm` / `eA2B10G10R10UnormPack32` case and `layer_squasher` skips its colour
   conversion. ~120 lines across `compositor.cpp`, `layer_squasher.cpp` and their shaders, and it
   is a *branch* in the compositor, which is the part of the server nobody wants to branch.
3. **Bypass the compositor's foveation entirely** (paper 6.8 says foveation is per-tile QP and
   resolution level, and "WiVRn NX's continuous foveation remap is bypassed when the codec is
   active"). This is option 2 plus disabling `foveation.cpp` for the eye streams and moving the
   render-cost win to VRS. ~250 lines, and it is the only option that actually delivers what
   6.8 describes.

**Recommendation: option 1 for Phase 3, option 3 for Phase 4.** Option 1 is what lets an
end-to-end session exist at all; the paper's own Phase 3 exit criterion is glass-to-glass
latency, not fovea quality.

### 1.4 Geometry and foveation, and how they reach the encoder

Two separate channels, both already in place:

* **Static, per session.** `struct encoder_settings` (`server/encoder/encoder_settings.h`):
  `width`, `height` (encoded size per eye), `src_layer`, `bit_depth`, `fps`, `bitrate`,
  `bitrate_multiplier`, `codec`, `encoder_name`, `options` (a
  `std::map<std::string,std::string>` of backend-specific keys — the codec's profile, tile size
  override and hybrid switch go here with no new plumbing), plus the four NX levers
  `sharp_text`, `foveation_foveal_qp`, `intra_refresh`, `ref_invalidation`. Built by
  `get_encoder_settings(vk_bundle &, wivrn_session &)` in `encoder_settings.cpp`, which is also
  where `select_encoder()` maps a configured encoder name and the headset's
  `supported_codecs` to a `{name, codec}` pair.

* **Per frame.** `to_headset::video_stream_data_shard::view_info_t`
  (`common/wivrn_packets.h:1227`) reaches the encoder through
  `video_encoder::encode(wivrn_session &, const view_info_t &, uint64_t frame_index)`:
  `XrTime display_time`, `std::array<XrPosef,2> pose`, `std::array<XrFovf,2> fov`,
  `std::array<foveation_parameter,2> foveation`, `bool alpha`, optional `quad_info_t`.
  `foveation_parameter` is a pair of `std::vector<uint16_t>` run lengths, one per axis, that map
  encoded pixels to render pixels.

  **This is the pose the codec's predictor needs, and it arrives one call too late.** `encode()`
  runs *after* `present_image()` has already submitted work, so a predictor that must warp the
  previous reconstruction by the pose delta needs the pose at present time. Two fixes:
  (a) plumb `view_info` into `present_image` — it exists in the compositor at
  `compositor.cpp:1039` (`images[i].view_info`), so this is a signature change on the pure
  virtual plus the four existing backends ignoring the new argument, ~25 lines; or (b) keep the
  pose ring on the encoder side and read `images[i].view_info` one frame late. (a) is correct,
  (b) is a hack. Do (a).

### 1.5 Output: where the bytes go today, and what the transport library replaces

Today, for an asynchronous encoder:

```
video_encoder::encode(cnx, view_info, frame_index)         // encoder thread (compositor::encoder_work)
  └─ virtual encode(slot, frame_index) -> optional<data>
     └─ sender::push(data)                                 // one queue+thread per socket
        └─ sender::run -> make_schedule(queue, data, queued)
           └─ video_encoder::SendData(span, end_of_frame, control, shard_pacer, spill_scheduler)
              ├─ fec::group_builder  (parity per group, adaptive ratio, interleave)
              ├─ shard_history       (retransmission ring, NACK answering)
              ├─ shard_pacer         (spread over a fraction of the frame period)
              ├─ spill_scheduler     (multipath: prefix on Wi-Fi, suffix on USB)
              └─ cnx->send_stream(shard)  -> UDP, 1400-byte payload
```

`to_headset::video_stream_data_shard` carries `stream_item_idx`, `frame_idx`, `shard_idx`,
optional `view_info` (first shard), optional `timing_info` (last shard), and a payload span.
`max_payload_size = 1400`.

Per paper 4.1 and 6.1 the codec's datagram is a **tile run with its own header**, not a slice of
an opaque byte stream. That is incompatible with `SendData`'s "cut this span into 1400-byte
pieces" contract. The split:

| Piece | Fate | Why |
|---|---|---|
| `video_encoder::SendData` byte-slicing | **replaced** for this codec | E5 already produces datagram-sized segments with headers; re-slicing them destroys the tile-run boundary |
| `shard_pacer` (`server/encoder/shard_pacer.h`, 295 lines) | **kept** | paper 4.2's row-band pipelining is a pacing schedule; the existing one already spreads over a configurable fraction of the frame period and is driven by the same measured frame rate |
| `spill_scheduler` / multipath | **kept**, unchanged | operates on byte offsets within a frame; tile runs are a prefix/suffix split just as well |
| `common/fec.h` `group_builder` / `rate_controller` (518 lines) | **kept**, re-parameterised | paper 4.4 wants *prioritised* FEC (more parity on the fovea bands). `fec::group_builder::set_layout(group_size, interleave_depth)` is already per-frame; making it per-band is ~40 lines |
| `shard_history` + NACK retransmission | **kept** | paper 4.5 replaces it *conceptually* with per-tile reference tracking, but the two are complementary: a retransmit that lands inside the deadline is strictly better than concealment. Keep both, and let the client stop NACKing once the reference epoch has moved on |
| `bitrate_controller` (`server/driver/bitrate_controller.h/.cpp`, AIMD/BBR, 2 s window, p90 utilisation, 250 ms evaluation) | **kept**, unchanged | paper 3.6 explicitly wants "the CPU's AIMD/BBR controller writes one number per frame, the byte budget". It already does exactly that through `video_encoder::set_bitrate` → `apply_bitrate` → `pending_bitrate` |
| `idr_handler` ladder (`server/encoder/idr_handler.{h,cpp}`, keyframe → intra refresh → ref invalidation) | **retired** per 6.6 | replaced by `nxwarp_idr_handler`, a `should_skip` that is always false and an `on_feedback` that folds the received-tile bitmap into the reference epoch |
| `encoder_watchdog` / failover | **kept** | it watches `encode_begin`/`encode_end` timestamps and is codec-agnostic |
| `video_encoder::set_intra_refresh` / `set_ref_invalidation` | **kept as no-ops** | they are headset toggles; the codec logs once that it recovers without them, as the Vulkan encoders already do |

Concretely, the new encoder overrides `encode()` to return `data{}` (empty, like x264 does) and
sends its own segments through a new sibling of `SendData`:

```cpp
// video_encoder.h, protected, next to SendData
void SendSegments(std::span<const std::span<uint8_t>> segments,
                  bool end_of_frame,
                  shard_pacer pacer = {},
                  spill_scheduler spill = {});
```

which reuses the FEC group builder, the history, the pacer and the spill scheduler verbatim and
skips only the slicing loop. **~150 lines added to `video_encoder.cpp`, ~10 changed.** The
mapped `HOST_VISIBLE|HOST_COHERENT|HOST_CACHED` output buffer of paper 3.6 becomes the backing
of those spans; `sendmmsg` is not used today (`wivrn_sockets.cpp` sends one datagram per call on
the server side), and moving to it is an independent optimisation worth ~100 lines in
`common/wivrn_sockets.cpp`.

### 1.6 The codec enum and capability negotiation

```cpp
// common/wivrn_packets.h:196
enum video_codec
{
    h264,
    h265,
    hevc = h265,
    av1,
    raw,
};
```

Proposed:

```cpp
enum video_codec
{
    h264,
    h265,
    hevc = h265,
    av1,
    raw,
    nxwarp,        // NX Warp, pure compute
    nxwarp_hybrid, // NX Warp enhancement over an HEVC base (paper 2.9 / 3.5)
};
```

Two enumerators rather than one profile field, because every consumer of the enum already
switches on it exhaustively and because the client's decoder factory has to build a completely
different object in the two cases (the hybrid one owns an `AMediaCodec` as well).

Negotiation is already capability-based and needs no new packet:

* client: `wivrn::decoder::supported_codecs()` (`client/decoder/decoder.cpp:71`) builds the list
  once from `android::decoder::supported_codecs()` / `ffmpeg::decoder::supported_codecs()` plus
  `raw`. NX Warp appends itself after a device probe (subgroup size, `shaderInt16`, ballot;
  paper 3.7's table) — ~40 lines in a new `client/decoder/nxwarp/nxwarp_probe.cpp`.
* the list travels in `from_headset::headset_info::supported_codecs`, documented "from preferred
  to least preferred".
* server: `select_encoder()` in `encoder_settings.cpp:243` walks
  `config.codec ? {*config.codec} : info.supported_codecs` per configured encoder. Adding a
  `check_nxwarp(codec)` probe next to `check_nvenc` / `check_vaapi` / `has_vk` is ~30 lines.
* `NLOHMANN_JSON_SERIALIZE_ENUM(video_codec, ...)` at `server/driver/configuration.cpp:56` gains
  `{nxwarp, "nxwarp"}, {nxwarp_hybrid, "nxwarp-hybrid"}` so the server config file can pin it.
* `to_headset::video_stream_description::codec` is a `std::array<video_codec,4>`, so the eyes can
  run NX Warp while the quad layer stays on HEVC. That falls out for free and is worth using:
  the promoted quad layer is a UI panel and is exactly the content the codec is worst at.

**Compatibility warning.** `serialization_traits<T, is_enum>` (`common/wivrn_serialization.h:493`)
feeds *every enumerator name and value* into the protocol type hash, and
`protocol_version = serialization_type_hash<protocol>(protocol_revision)`. Adding an enumerator
therefore changes `protocol_version` and every existing client and server refuses the handshake.
For a fork where both ends ship together this is correct behaviour, not a problem — but it must
be a deliberate, simultaneous release of client and server, and it cannot be A/B tested against
an unmodified peer.

### 1.7 Motion smoothing hands over (paper 6.11)

`server/compositor/motion_estimator.{h,cpp}` builds a downscaled luma pyramid of the previous
composited frame and block-matches against it, producing a `motion_field_data`
(`common/motion_field.h`) that either drives `server/compositor/motion_warper.cpp` (server mode)
or is sent to the headset and consumed by `reprojection.glsl`'s displacement path (headset mode).
It is invoked at `compositor.cpp:973` `update_motion_field(display_time, frame_index, live_src,
live_src_rect, live_flip_y)`, always against the live composited views.

Per 6.11 the estimator is retired when the codec is active and the codec's E1 per-tile vectors
become the extrapolation field, with `STATIC_MV` tiles excluded. The field format is the same
shape (a small two-layer texture of 2D vectors), so the client side does not change at all:
`stream_defoveator::motion_warp { const motion_field_data * field; float step; }` keeps working.
Work is on the server: emit `motion_field_data` from the encoder's tile-mode buffer instead of
from the estimator, ~80 lines, and skip `update_motion_field` when the codec is active, 3 lines.
This deletes a whole GPU pass from the compositor, which is a real win on the RX 580 class of
machine.

### 1.8 Server totals

| Area | New | Changed |
|---|---|---|
| `video_encoder_nxwarp.*`, `nxwarp_idr_handler.*` | ~940 | — |
| `nx_codec` static library linkage, CMake | ~60 | ~15 |
| `video_encoder.{h,cpp}` (`SendSegments`, enum, factory, `view_info` in `present_image`) | ~150 | ~45 |
| `encoder_settings.cpp` (probe + selection) | ~30 | ~10 |
| `compositor.cpp` (uint view formats, motion estimator bypass) | — | ~25 |
| `wivrn_vk_bundle.cpp` (dedicated compute queue) | ~30 | ~10 |
| `configuration.cpp`, packets | — | ~10 |
| colour-space option 1 | — | 0 |
| colour-space option 3 (Phase 4) | ~120 | ~130 |

**~1210 new, ~115 changed for Phase 3.**

---

## 2. Client (Android)

### 2.1 Where `decoder_nxwarp` plugs in

Same shape as the server: one factory and one interface.

```cpp
// client/decoder/decoder.h
class decoder
{
public:
    struct blit_handle
    {
        wivrn::from_headset::feedback feedback;
        wivrn::to_headset::video_stream_data_shard::view_info_t view_info;
        vk::ImageView image_view = nullptr;
        vk::Image image = nullptr;
        vk::Extent2D extent{};
        vk::ImageLayout & current_layout;
        vk::Semaphore semaphore = nullptr;
        uint64_t * semaphore_val = nullptr;
    };

    static std::shared_ptr<decoder> make(vk::raii::Device &, vk::raii::PhysicalDevice &,
                                         uint32_t vk_queue_family_index,
                                         const to_headset::video_stream_description &,
                                         uint8_t stream_index,
                                         std::weak_ptr<scenes::stream>,
                                         shard_accumulator *);
    virtual void push_data(std::span<std::span<const uint8_t>> data,
                           uint64_t frame_index, bool partial) = 0;
    virtual void frame_completed(const from_headset::feedback &,
                                 const to_headset::video_stream_data_shard::view_info_t &) = 0;
    virtual vk::Sampler sampler() = 0;
    static const std::vector<video_codec> & supported_codecs();
};
```

`decoder::make` is a `switch (description.codec[stream_index])`; two new cases and two new files:

| File | Lines | Content |
|---|---|---|
| `client/decoder/nxwarp/nxwarp_decoder.{h,cpp}` | ~600 | ring, Pass A/B/C submits, per-tile metadata SSBO, telemetry |
| `client/decoder/nxwarp/nxwarp_hybrid.{h,cpp}` | ~350 | subclass that owns the `AMediaCodec` base layer |
| `client/decoder/nxwarp/nxwarp_probe.cpp` | ~40 | capability probe for `supported_codecs()` |

`client/decoder/raw_decoder.{h,cpp}` (69 + 347 lines) is the template: it already owns a pool of
`image_allocation`s with per-image `vk::raii::Semaphore` + `semaphore_val`, a `VkSamplerYcbcrConversion`,
its own command pool, and it pushes `blit_handle`s into the scene. `push_data` accumulates into a
`buffer_allocation`, `frame_completed` submits and publishes. NX Warp's `push_data` instead
copies tile runs into the staging ring and, once a band is complete, kicks Pass A/B for that
band — which is where the pipelining of paper 4.2 actually happens on the client.

### 2.2 How a decoded frame reaches the reprojection pass today

```
decoder::frame_completed
  └─ scenes::stream::push_blit_handle(shard_accumulator *, shared_ptr<blit_handle>)   [stream.cpp:652]
     └─ decoders[stream].latest_frames[frame_index % image_buffer_size] = handle      (frames_mutex)
...render thread, once per xrWaitFrame:
  scenes::stream::common_frame(XrTime display_time)                                   [stream.cpp:702]
     └─ picks the newest frame index present in *every* stream, nearest
        (display_time - dejitter.delay_ns())
  → current_blit_handles[]
  → barrier to eGeneral if current_layout == eUndefined                               [stream.cpp:1084]
  → stream_defoveator::defoveate(cmd, input{rgb view, sampler, rect, layout, ...},
                                 post_processing, motion_warp)                        [stream_defoveator.cpp]
     └─ client/shaders/reprojection.glsl (534 lines): defoveation from the
        foveation_parameter runs, CAS or FSR, comfort vignette, ambient glow, deband
  → xrEndFrame with the projection layer
```

Key facts for the codec:

* `image_buffer_size = 3` (`client/scenes/stream.h:63`), not 4. Paper 4.3 wants a **four-slot
  ring** with `slot = N mod 4`. Changing the constant to 4 is one line, and it is safe: the
  array is indexed modulo its own `size()` everywhere. But note the two rings are not the same
  thing — `latest_frames` holds *published* frames for the render thread to choose from, while
  paper 4.3's ring is the *decoder's* reference ring. The codec needs both: its own 4 reference
  images inside `nxwarp_decoder` (paper 6.6: "the client holds a four-slot reference ring in
  display format"), and it publishes into `latest_frames` as every decoder does. Roughly
  4 × 2048 × 2048 × 4 B = 67 MB per eye for RGBA8 references, 134 MB for two eyes, which on a
  Pico 4 (8 GB, ~3 GB usable to an app) is affordable but is the single largest allocation the
  client would make.
* The `blit_handle` is handed to the reprojection pass as `vk::ImageView` + `vk::Sampler`, where
  the sampler today carries a `VkSamplerYcbcrConversion`. Paper 3.10 says the decoder's output is
  "a plain RGBA storage image, replacing the YCbCr sampler path". That works with **no change to
  `reprojection.glsl`**: `decoder::sampler()` returns a plain `vk::Sampler` and the shader's
  `texture()` calls are format-agnostic. `stream_defoveator::ensure_pipeline(view, rgb, a)`
  already rebuilds the pipeline per sampler pair, and the descriptor set layout embeds the
  sampler through `pImmutableSamplers`, so a different sampler is a pipeline rebuild at stream
  start and nothing else. **Zero shader lines**, assuming colour-space option 1 above is *not*
  chosen (with option 1 the decoder's output is YCbCr and the existing path is kept verbatim,
  which is even less work).

### 2.3 Per-tile metadata and the deadline (paper 4.3)

Paper 4.3 asks for a per-slot SSBO of 4-byte entries `{pose_seq, age, state}` and a reprojection
that warps each output tile from its own `pose_seq`. Where it lands:

* the SSBO is owned by `nxwarp_decoder` and exposed on `blit_handle` as one extra field
  (`vk::Buffer tile_meta; uint32_t tile_stride;`) — 2 lines in `decoder.h`, and every other
  decoder leaves them null.
* `reprojection.glsl` gains a per-tile pose lookup. Today it takes one `rgb_rect`/`scale`/`bias`
  push constant for the whole view; per-tile poses mean a storage buffer read per fragment plus a
  per-tile homography. **~90 shader lines and ~120 lines in `stream_defoveator.cpp`** (a second
  descriptor binding, a specialization constant so the existing path compiles out unchanged, and
  the buffer plumbing). This is the only place the client's hot pixel path grows, and it must be
  behind a specialization constant: `reprojection.glsl` already carries five of them for exactly
  this reason (`alpha`, `do_srgb`, `cas_full_kernel`, `fsr`, …), and the file's own comments are
  explicit that every extra tap costs on Adreno.
* the deadline wake-up of 4.3 step 4 is `scenes::stream`'s existing frame loop plus
  `client/decoder/dejitter.h` (`dejitter_buffer`, an adaptive playout delay already fed one
  sample per arriving eye-zero frame and already configurable live). Paper 4.3's "move the
  deadline 1 ms earlier after 5 bad frames, relax 0.2 ms per clean second" is a different control
  law in the same object: **~60 lines in `dejitter.h`**, and the existing `config.dejitter`
  toggle already gates it.
* concealment (4.3 step 3) is a third dispatch in `nxwarp_decoder`, counted in its 600 lines.

### 2.4 Hybrid mode coexistence

`client/decoder/android/android_decoder.{h,cpp}` (137 + 590 lines) already does everything the
hybrid base layer needs, and does it the way paper 3.5 prescribes:

* `AImageReader` with `image_buffer_size + 4` max images, its `ANativeWindow` as the codec's
  output surface;
* `map_hardware_buffer(AImage *)` → `vkGetAndroidHardwareBufferPropertiesANDROID`, a `VkImage`
  with `VkExternalFormatANDROID`, `VkImportAndroidHardwareBufferInfoANDROID`, and a
  `VkSamplerYcbcrConversion` on the external format;
* `std::unordered_map<AHardwareBuffer *, shared_ptr<mapped_hardware_buffer>>` — the import cache
  paper 3.5 asks for, already there;
* `on_media_input_available` / `on_media_output_available` async callbacks, a `sync_queue` of
  input buffers, one worker thread.

So `nxwarp_hybrid` should **contain** an `android::decoder` rather than reimplement it: give
`android::decoder` a mode in which it publishes its `mapped_hardware_buffer` to a callback
instead of building a `blit_handle`, and let the hybrid decoder run Pass C against it. ~60 lines
changed in `android_decoder.{h,cpp}`, ~350 new in `nxwarp_hybrid.cpp`.

Two gaps against the paper: the current decoder does **not** set `KEY_LOW_LATENCY` or the
Qualcomm `vendor.qti-ext-dec-low-latency.enable` key (grep finds neither), and it uses
`AImageReader_acquireNextImage`-style acquisition rather than `acquireLatestImageAsync` with a
sync fd imported into a `VkSemaphore`. Both are on the critical path for paper 3.5's latency
claim and both are small (~40 lines together) — and both should be done *before* the codec work,
because they would improve the HEVC path today and would let Phase 0 measure K6 honestly.

### 2.5 The Android UDP receive path

There is no separate video-receive thread. One thread does everything:

* `scenes::stream::network_thread` (`client/scenes/stream.h:109`) runs a visitor loop over
  `wivrn_session::poll(visitor, timeout)` (`client/wivrn_client.h:300-420`).
* `poll` drains `stream.receive_pending_lossy()` (already-buffered datagrams), then the control
  TCP socket, then the secondary (USB/TCP) path, then `::poll()` on three fds, then
  `stream.receive_lossy()`.
* the actual read is `wivrn::UDP::receive_raw()` (`common/wivrn_sockets.cpp:295`):
  **`recvmmsg` is already used**, `num_messages = 20`, `message_size = 2048`, flags
  `MSG_DONTWAIT | MSG_TRUNC`, into one `shared_ptr<uint8_t[]>` of 40 KB reused across calls
  unless the previous batch is still referenced. Datagrams larger than 2048 are counted and
  dropped. Decryption is in-place per message.
* the visitor for a video shard is `scenes::stream::operator()(video_stream_data_shard &&)`
  (`client/scenes/stream_network.cpp:230`), which routes to
  `shard_accumulator::push_shard` → `frame_window<shard_set, 6, 3>` → `try_submit_front` →
  `decoder::push_data`.

So `decoder_nxwarp::push_data` is called **on the network thread**, synchronously, per shard
batch. That is the right place to copy a tile run into the staging ring (a memcpy of ≤1400 bytes)
but it is the wrong place to submit GPU work if the submit can block on `vk.queue`. The Pass A/B
submit for a completed band should go to a small worker thread, as `android::decoder` already
does with its `utils::sync_queue<std::function<bool(void)>> jobs` + `std::thread worker`. Copy
that pattern.

Changes needed for the codec: `message_size` stays 2048 (paper 4.1's datagrams are MTU-sized);
`num_messages` should rise from 20 to 64 — at 400 Mbit and 1400-byte datagrams a 90 Hz frame is
~400 datagrams, and 20 per syscall is 20 syscalls per frame per batch. **1 line**, worth
measuring. `SO_RCVBUF` is already 5 MB (`client/wivrn_client.cpp:57`,
`stream.set_receive_buffer_size(1024 * 1024 * 5)`), which is about nine frames at 400 Mbit —
ample, and nothing to change there.

### 2.6 The settings toggle

Every NX feature has a headset toggle; the codec is not exempt. The three files, in order:

1. `client/configuration.h` — one field next to the other stream levers (`fec`, `dejitter`,
   `motion_smoothing`, …). Proposed:
   ```cpp
   // NX Warp: which decoder the headset asks for. auto = the server decides from the
   // capability list, which is what everyone should leave it on.
   enum class nxwarp_mode { off, automatic, compute, hybrid };
   nxwarp_mode nxwarp = nxwarp_mode::automatic;
   ```
2. `client/configuration.cpp:347` — one entry in the serialization table
   (`scalar("nxwarp", &configuration::nxwarp)`).
3. `client/scenes/gui_settings.cpp` — one `list.push_back({...})` with
   `.ui = ui_kind::combo`, `.get_int/.set_int`, `.options`, `.title`, `.default_int`, and
   `ctx.on_streaming_changed()` in the setter so the change forces a re-handshake. It belongs in
   the **"Advanced — niche and experimental options"** section (`sec_advanced`, line ~753) for
   Phase 3 and moves up to the video section when it is the default. The `motion_smoothing`
   entry at line 733 is the exact template, including the multi-paragraph `.description` that
   explains the trade.

The value reaches the server by re-ordering `supported_codecs` in
`from_headset::headset_info` (built in `client/scenes/stream.cpp` around line 361, next to
`info.settings.motion_smoothing`), which requires **no protocol change at all**: `off` removes
the NX Warp entries from the list, `compute`/`hybrid` moves one of them to the front,
`automatic` leaves the probe's own order. That is a strictly better design than a new settings
field, because it reuses the negotiation that already exists.

`from_headset::settings_changed` (`common/wivrn_packets.h:402`) is where the *live* toggles live
(bitrate, FEC, pacing, retransmit, intra refresh, …); the codec choice is not live — it needs a
new encode session — so it belongs in the handshake, not there.

### 2.7 Client totals

| Area | New | Changed |
|---|---|---|
| `nxwarp_decoder.*`, `nxwarp_hybrid.*`, probe | ~990 | — |
| `decoder.{h,cpp}` factory + `blit_handle` fields | — | ~20 |
| `android_decoder.{h,cpp}` (hybrid hook, low-latency keys, sync fd) | — | ~100 |
| `reprojection.glsl` + `stream_defoveator.cpp` (per-tile poses) | ~90 shader | ~120 |
| `stream.h` / `stream.cpp` (ring depth, plumbing) | — | ~25 |
| `dejitter.h` (deadline policy) | ~60 | ~10 |
| `wivrn_sockets.cpp` (recvmmsg batch size) | — | ~2 |
| settings toggle | ~35 | ~10 |

**~1175 new, ~290 changed.**

---

## 3. Does the Pico client re-run per vsync with `discard_frame = false`?

**Yes, and it matters more than it looks.**

`client/hmd_traits.h:57` declares `bool discard_frame = true; // can do xrBeginFrame twice to
discard the first one`, and `client/hmd_traits.cpp:213` sets `discard_frame = false` for every
`manufacturer == "Pico"` device (alongside `view_locate = false`).

The consumer is `client/scenes/stream.cpp:1147`:

```cpp
if ((not application::get_hmd_traits().discard_frame) or
    std::ranges::any_of(current_blit_handles, [](const auto & h){ return h and h->feedback.times_displayed < 2; }) or
    motion_available or
    is_gui_interactable() or
    state_ == state::reconnecting)
{
    ... defoveation + reprojection + xrEndFrame with a real projection layer ...
}
```

On a headset that *can* discard, WiVRn skips the whole reprojection pass on any refresh that
would merely redisplay a frame it has already shown twice, and lets the runtime's own timewarp
repeat the image. On Pico the first disjunct is unconditionally true, so **the defoveation +
reprojection pass runs on every single vsync**, whether or not a new frame arrived — 90 or 120
times a second, over the full defoveated eye images, including CAS/FSR, vignette, glow and
deband.

Consequences for a decoder that must run once per frame:

1. **The decode budget and the reprojection budget are not the same budget, and on Pico the
   second one is charged at the refresh rate.** Paper 3.1 books "reprojection + compositor, about
   2 to 3 ms" per vsync and then asks for 5 ms p50 of decode. On Pico those 2–3 ms are paid
   90 times a second regardless of stream rate, so a decoder that runs at 45 Hz (fps divider,
   space warp) still leaves only ~8 ms of the 11.1 ms period for itself on the frames where it
   does run. The p99 number of 7 ms is against 8, not against 9.
2. **Paper 4.3's per-tile pose lookup is charged per vsync, not per decoded frame.** That is
   arguably good — it is precisely the case 4.3 is designed for, since a repeated frame warped
   to the new display pose is better than a repeated frame — but it means the extra shader taps
   land on the hottest pass on the weakest device. Hence the specialization constant in 2.3: the
   per-tile path must compile out entirely when the codec is not active.
3. **Decode and reprojection must not share a queue submission.** They run at different rates.
   The decoder publishes into `latest_frames` and the render thread picks with `common_frame`;
   that decoupling already exists and must be preserved — a decoder that tries to render
   in-line with `xrEndFrame` would inherit the vsync rate.
4. There is a *positive* consequence too: because the pass runs every vsync anyway, the
   concealment warp of paper 4.3 costs nothing extra in scheduling terms on Pico. The dispatch
   is already happening; only its content changes.

---

## 4. Windows port

`wivrn-nx-windows` is a SteamVR driver (`shim/`) plus a helper process (`helper/`) talking over a
named pipe with a frozen contract in `ipc/wivrnnx_ipc.h`. The video path is:

```
shim/src/direct_mode.cpp  (SteamVR hands ID3D11Texture2D per eye)
  → shim/src/frame_staging.h  (a ring of shared NT-handle textures, keyed mutex)
  → ipc FrameReady { frame_id, sample_time_qpc, predict_s, pose_q[4], pose_p[3] }
  → helper/src/video_intake.cpp  (one encoder thread, newest-frame-wins, no queue)
     → helper/src/encoder/video_encoder.h : IVideoEncoder
        → helper/src/encoder/d3d11_stage.cpp  (helper's own D3D11 device, opens the ring)
        → helper/src/encoder/amf_video_encoder.cpp (AMF H.264/H.265)
     → helper/src/video_bridge.cpp → shards → the same client
```

The seam for NX Warp is `IVideoEncoder`:

```cpp
class IVideoEncoder
{
    virtual bool configure(const ipc::StagingConfig & staging, uint32_t vrserver_pid,
                           const EncoderConfig &) = 0;
    virtual void shutdown() = 0;
    virtual void set_bitrate(uint32_t bitrate_bps) = 0;
    virtual EncoderStreamInfo stream_info() const = 0;
    virtual EncodeResult encode(const ipc::FrameReady & frame, bool force_idr,
                                std::vector<EncodedFrame> & out) = 0;
};
```

A `VulkanNxWarpEncoder : IVideoEncoder` sits next to `AmfVideoEncoder`, sharing the same
`D3D11Stage` for the ring. What has to be built, and it is more than on Linux because the helper
has **no Vulkan at all today** (no instance, no device, no VMA):

| Piece | Lines | Notes |
|---|---|---|
| `helper/src/vk/vk_context.{h,cpp}` | ~350 | instance + device + queue + allocator, mirroring `server/utils/wivrn_vk_bundle.*`. Required extensions: `VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32`, `VK_KHR_win32_keyed_mutex`, plus `shaderInt16`/subgroup features |
| `helper/src/encoder/d3d11_vk_interop.{h,cpp}` | ~300 | the paper 3.8 path, see below |
| `helper/src/encoder/vk_nxwarp_encoder.{h,cpp}` | ~450 | the `IVideoEncoder` wrapper around `nx_codec`'s encoder |
| CMake, mingw-w64 Vulkan headers/loader | ~60 | the tree already builds with mingw (`d3d11_1.h`/`d3d11_4.h` comments) |

The D3D11→Vulkan interop, concretely, and where it differs from paper 3.8:

* Paper 3.8 assumes "SteamVR's texture is not created with sharing flags we control, so copy it
  into our own shared texture". **That copy already exists in this tree**: `shim/src/frame_staging.h`
  creates the ring with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | ..._SHARED_KEYEDMUTEX` and the
  shim copies SteamVR's texture into a slot. So the helper does not need a second copy — it
  imports the *existing* staging texture.
* `D3D11Stage::open_ring()` already does `ID3D11Device1::OpenSharedResource1` on duplicated NT
  handles. For Vulkan the same duplicated handle is imported with
  `VkImportMemoryWin32HandleInfoKHR{ handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
  handle }` bound to a `VkImage` created with `VkExternalMemoryImageCreateInfo`, format matched to
  `D3D11Stage::dxgi_format()` (today `DXGI_FORMAT_R8G8B8A8_UNORM` / `_TYPELESS`), and validated
  with `vkGetPhysicalDeviceImageFormatProperties2` before the ring is accepted.
* **Synchronisation is already decided by the frozen contract and it is the keyed mutex, not a
  fence.** `d3d11_stage.h`'s header comment: "the shim releases with key 1 when a frame is in a
  slot, the helper acquires key 1, encodes, and releases with key 0". Vulkan can join that
  protocol directly with `VkWin32KeyedMutexAcquireReleaseInfoKHR` chained onto the encode
  `VkSubmitInfo` — acquire key 1, release key 0 — which is precisely paper 3.8's *fallback*
  path. The shared-fence path (`ID3D11Device5::CreateFence(D3D11_FENCE_FLAG_SHARED)` imported as
  a timeline semaphore) would be an improvement but would change `ipc/wivrnnx_ipc.h`, which the
  repo documents as frozen. **Use the keyed mutex; it is both less work and contract-compliant.**
* The RX 580 note in paper 3.8 holds: GCN4 wave64 on the Windows AMD driver, same SPIR-V as
  RADV, so the encoder shader binaries are shared with the Linux build and only the host code
  differs.

Second, smaller difference from the Linux side: the helper's colour path is RGBA from SteamVR,
not YCbCr. So the Windows port is the *easier* target for the codec's native RGB pipeline (1.3
option 2/3 comes free there), and the two ends will disagree about colour until Linux follows.
That is an argument for doing the Windows encoder second and the Linux compositor change first,
not the other way round.

**Windows totals: ~1160 new, ~40 changed.**

---

## 5. Risks

Ordered by how likely they are to change the plan rather than by severity.

1. **Colour space (1.3).** The Linux server hands the encoder 4:2:0 YCbCr, the Windows helper
   hands it RGBA, and the paper assumes RGB. Until this is decided the encoder cannot be written
   once. *Mitigation: decide before Phase 3 starts; option 1 for Phase 3.*
2. **Enum change breaks the protocol hash (1.6).** Every client and server must ship together.
   No mixed-version testing is possible. *Mitigation: land the enumerators early, in a release
   where they are inert, so the version break is paid once and not at integration time.*
3. **`vk.queue.mutex` is Monado's queue mutex (1.2).** Eight submits per frame per stream
   serialise against the compositor. *Mitigation: batch, or take a dedicated compute queue.*
4. **The pose arrives after the image (1.4).** `present_image` has no `view_info`. A predictor
   needs it. *Mitigation: change the pure virtual signature; four backends ignore the argument.*
5. **Pico runs the reprojection pass every vsync (section 3).** The per-tile pose lookup lands
   on the hottest pass on the weakest GPU. *Mitigation: specialization constant, and measure the
   pass cost with and without before committing to per-tile poses.*
6. **Four full-size reference images per eye is 134 MB** on a device with ~3 GB of app budget,
   on top of the decoder's coefficient buffers. *Mitigation: paper 6.6's ring is normative, so
   the fix is the Lite profile's half-resolution residual, or references in RGB10A2 rather than
   RGBA16.*
7. **MediaCodec low-latency keys are not set today (2.4).** The hybrid path's latency floor is
   unmeasured on Pico firmware and the current code cannot measure it. *Mitigation: set the keys
   and measure on the HEVC path first; it is a win regardless of the codec.*
8. **`shard_accumulator`'s `frame_window<6,3>` assumes frames are decoded whole and in order**
   ("a decoder cannot be fed out of order, so a newer frame that completed first waits its
   turn"). The codec explicitly decodes partial frames and wants tiles that arrive after the
   deadline. *Mitigation: `push_data(..., bool partial)` already exists and is already called
   with `partial = true`; but `try_submit_front`'s retirement policy needs a per-tile notion of
   "done", ~80 lines in `shard_accumulator.cpp` not counted above.*
9. **FEC and per-tile reference tracking both spend bandwidth on the same loss.** Paper 4.4 says
   keep FEC deliberately small; the existing adaptive controller will happily take 25% at 4+1
   under loss, which would fight the codec's own concealment. *Mitigation: cap
   `fec::rate_controller`'s group size when the codec is active.*
10. **The watchdog will fail the codec into a fallback encoder mid-session.** `encoder_watchdog`
    swaps a wedged encoder for another, and `watchdog.set_eligible()` is currently false only for
    x264 and raw. A compute encoder that stutters once on a driver hiccup would be replaced by
    VAAPI and the client's decoder would be wrong. *Mitigation: `set_eligible(false)` for
    NX Warp until failover can renegotiate the codec, which it cannot today.*
11. **No `sendmmsg` on the server (1.5).** Paper 3.6's 300 µs CPU budget assumes it. Today it is
    one `sendto` per datagram. *Mitigation: independent work item, ~100 lines.*
12. **Quad layer content.** The promoted quad layer stream is text and UI — the codec's worst
    case. *Mitigation: leave stream 3 on HEVC; `video_stream_description::codec` is already
    per-stream.*

---

## 6. Capturing test material

The capture patch lives on branch **`nx-warp-capture`** of the `wivrn-nx` repository
(worktree at `/run/media/nerdrx/Lex/claude/wivrn-nx-capture`). It is off by default, changes no
protocol and adds no codec; with the environment variable unset it is a branch on a null pointer.

New: `server/encoder/raw_dump.{h,cpp}`. Hooked in `video_encoder::present_image` (submit the
copy) and `video_encoder::encode` (wait for it and write, which is where `view_info` exists).

### Usage

```sh
mkdir -p /path/to/capture
WIVRN_RAW_DUMP=/path/to/capture \
WIVRN_RAW_DUMP_FRAMES=600 \
WIVRN_RAW_DUMP_MAX_MB=16384 \
  wivrn-server
```

| Variable | Default | Meaning |
|---|---|---|
| `WIVRN_RAW_DUMP` | unset | directory to write into; must already exist. Enables the tap |
| `WIVRN_RAW_DUMP_FRAMES` | 300 | frames per stream; a cap, not a target |
| `WIVRN_RAW_DUMP_MAX_MB` | 8192 | total budget across every stream |

Output, per stream (0 = left eye, 1 = right eye):

```
stream0.yuv          raw frames, appended, no container, no padding
stream0.jsonl        one JSON object per frame in stream0.yuv
stream0-info.json    geometry and pixel format of stream0.yuv
```

A `.jsonl` line:

```json
{"frame":10423,"stream":0,"display_time_ns":88451234567,"width":2048,"height":2048,
 "alpha":false,
 "pose":[{"orientation":[x,y,z,w],"position":[x,y,z]},{...}],
 "fov":[{"tan_left":-1.02,"tan_right":0.98,"tan_up":1.05,"tan_down":-1.05},{...}],
 "foveation":[{"x":[1,4,5,3,1],"y":[...]},{...}]}
```

`pose`, `fov` and `foveation` are both eyes on every line (they come from one `view_info_t`), so
the left and right `.jsonl` agree frame for frame and either can be used alone.
`fov` is stored as **tangents**, not angles.

### Format, and why it is not 4:4:4

The `.yuv` holds the encoder's own input, copied plane for plane with no conversion:

* 8 bit: **NV12** — a `W × H` 8-bit Y plane followed by a `W/2 × H/2` interleaved CbCr plane
  (`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`);
* 10 bit: **P010LE** — the same shape in 16-bit little-endian samples whose top 10 bits carry the
  value (`VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16`).

The chroma really is 4:2:0 at this point in WiVRn's pipeline (`layer_squasher` + `foveation`
write the two planes directly), so upsampling to 4:4:4 inside the tap would invent precision that
does not exist and double the bytes going to disk at 90 frames a second. `-info.json` names the
format, plane sizes and frame stride exactly; the offline harness converts where the conversion
can be reasoned about. To play a capture back directly:

```sh
ffplay -f rawvideo -pixel_format nv12 -video_size 2048x2048 stream0.yuv
```

The image is **foveated**, because that is what the encoder sees. The `foveation` runs on each
`.jsonl` line are what maps an encoded pixel back to render space (they are the same
`foveation_parameter` the headset uses to defoveate, read as: "the first output pixel has 3
source pixels, the next 4 have 2, then 5 have 1, …"). For material without foveation, run the
server with foveation disabled in its configuration.

### Limitations

* **Eye streams only.** The alpha stream is a luma mask and the quad stream is a UI panel;
  neither is codec test material.
* **The image must arrive in `eGeneral`, on the compositor or transfer queue family.** That
  covers `vaapi`, `nvenc` and `x264`. The Vulkan video encoders move the image into a
  video-encode layout on an encode queue, which a transfer cannot read from; those log once and
  do not dump.
* No queue-family ownership transfer is performed: the compositor has already released the image
  to the encoder's family and the tap submits on a queue of that same family, waiting on the same
  compositor semaphore value the encoder waits on.
* At 2048×2048 8-bit, one frame per eye is 6 MB, so 300 frames of both eyes is 3.6 GB and about
  3.3 seconds of 90 Hz motion. Capture short, deliberate sequences (a yaw sweep, a pitch sweep,
  a static hold, one with fast object motion) rather than long sessions.
* Writing is synchronous on the encode thread. The session *will* drop frames while dumping.
  That is fine for capture and is why it is off by default.

### Build

```sh
cmake -S . -B build-server -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_PREFIX_PATH=/path/to/local -DWIVRN_BUILD_CLIENT=OFF
cmake --build build-server -j4
```

Verified: the branch builds clean (`server/wivrn-server` links) with NVENC, VAAPI, x264 and
Vulkan video encode all enabled.
