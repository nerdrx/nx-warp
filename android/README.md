# NX Warp Android client shell

The headset-side test vehicle for NX Warp, ahead of WiVRn NX integration.

It receives NX Warp datagrams over UDP, depacketizes them, places tiles into the
4-slot frame ring, runs the deadline state machine, sends per-band feedback
upstream, and presents the result with a debug HUD. The **decoder is a
placeholder** that paints tiles by their presentation state; the real Vulkan
decoder (`vk/`) drops into a documented seam without touching anything else.

**No OpenXR.** This app opens a plain fullscreen Vulkan 1.1 swapchain.
Frameless presentation lives inside WiVRn's reprojection pass (PAPER 4.3), so
OpenXR arrives with the WiVRn NX integration, not here.

What this app is *for*: measuring the receive path. PAPER 4.1 claims the Android
UDP receive path "tops out between 50k and 100k packets/s even with `recvmmsg`",
and PAPER 4.11 puts saturation "around 80k pps" on one core of an XR2. Those
numbers decide whether 1 Gbit/s over WiFi is reachable. They have never been
measured on a real device. `android/tools/nxvc-blast` plus the self-test mode
measure them.

---

## Layout

```
android/
  CMakeLists.txt          native build; compiles the shaders to .spv.h headers
  build.gradle            AGP 8.13, arm64 only, NativeActivity, no Java sources
  AndroidManifest.xml
  shaders/
    tiles.comp            placeholder Pass B: paint tiles by state, 4:2:0 out
    present.comp          2-plane YCbCr -> RGBA for the RGB swapchain
    hud.comp              5x7 bitmap font overlay
    nxc_common.glsl       per-tile metadata accessors (TRANSPORT.md 7.3)
  src/
    nxc_app.cpp           entry point, decode thread, telemetry, HUD, self test
    nxc_net.{h,cpp}       receive thread: recvmmsg, SO_RCVBUF, FIFO, affinity
    nxc_ring.h            SPSC ring that recvmmsg writes into directly
    nxc_transport.h       >>> THE TRANSPORT SEAM <<<
    nxc_transport_stub.cpp  stands in for transport/ until it exists
    nxc_wire.h            TRANSPORT.md 2/3/8 transcribed; deleted at the swap
    nxc_frame_ring.{h,cpp}  4-slot ring, per-tile metadata, deadline machine
    nxc_decoder.h         >>> THE DECODER SEAM <<<
    nxc_decoder_placeholder.cpp
    nxc_vk.{h,cpp}        Vulkan 1.1 swapchain, dispatches, blit, timestamps
    nxc_font.{h,cpp}      5x7 font and the HUD character grid
  tools/nxvc-blast/       host-side flooder (Linux, standalone)
  tests/                  host-side loopback check (no device needed)
```

---

## Building

Toolchain used, all under `/run/media/nerdrx/Lex/claude/tools`:

| Component | Version |
|---|---|
| Android SDK | platform 34, build-tools 34.0.0 |
| NDK | 29.0.14206865 |
| CMake | 3.31.5 (SDK) |
| JDK | 21.0.12+8 |
| Gradle | 8.13 (wrapper) |
| AGP | 8.13.0 |

`local.properties` already points `sdk.dir` at the SDK above.

### The APK

```sh
cd android
export JAVA_HOME=/run/media/nerdrx/Lex/claude/tools/jdk-21.0.12+8
export ANDROID_HOME=/run/media/nerdrx/Lex/claude/tools/android-sdk
chrt -i 0 taskset -c 8-11 nice -n 19 ./gradlew assembleDebug --no-daemon
```

Output: `build/outputs/apk/debug/nxwarp-client-debug.apk` (about 5.4 MB, arm64
only, native symbols kept).

The `chrt -i 0 taskset -c 8-11 nice -n 19` prefix is not decoration: this tree is
shared with encoder benchmarks and a live desktop session. The affinity mask also
bounds the Ninja jobs the AGP build spawns, which is the only reliable way to cap
them, and `org.gradle.workers.max=4` is set in `gradle.properties`.

### The blaster (host)

```sh
cd android/tools/nxvc-blast
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
```

Standalone by design: no SDK, no NDK, no Vulkan. Linux only (`sendmmsg`).

### The host loopback check

```sh
cd android/tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
```

This links the **real** `nxc_transport_stub.cpp` and `nxc_frame_ring.cpp` and
runs them against real blaster traffic on `127.0.0.1`. It catches the expensive
failure mode — the blaster and the depacketizer disagreeing about the wire
format, which on a headset looks like "everything is dropped" with no clue why.

```sh
./build/host-loopback --seconds 8 &
../tools/nxvc-blast/build/nxvc-blast --host 127.0.0.1 --profile 150mbit --seconds 6
```

---

## Installing and running

```sh
adb install -r build/outputs/apk/debug/nxwarp-client-debug.apk
adb shell am start -n org.nxwarp.client/android.app.NativeActivity
adb logcat -s nxwarp
```

The app listens on **UDP 9944** and learns its peer from the first datagram, so
no address configuration is needed on the device. Feedback packets and the
once-a-second stats report both go back to that learned peer.

---

## The pps / throughput self test

This is the procedure that produces the number PAPER 4.1 and 4.11 assert.

1. Put the device and the host on the same network. For a clean measurement use
   USB tethering or a quiet 5/6 GHz link; a congested AP measures the AP.
2. Keep the screen on and the app foregrounded. WiFi power save adds 100 ms
   (PAPER 4.11) and Android will park background sockets.
3. Start the app, confirm from `adb logcat -s nxwarp` that it bound the port and
   check the granted `SO_RCVBUF` line.
4. Run the blaster from the host:

```sh
# The paper's three operating points.
./nxvc-blast --host <device-ip> --profile 150mbit --seconds 30   # ~13.5 kpps
./nxvc-blast --host <device-ip> --profile 400mbit --seconds 30
./nxvc-blast --host <device-ip> --profile 1gbit   --seconds 30   # ~90 kpps

# Jumbo USB path: pps falls about 6x for the same bitrate (PAPER 4.1).
./nxvc-blast --host <device-ip> --profile jumbo --seconds 30

# Find the knee automatically: ramps until the device reports loss.
./nxvc-blast --host <device-ip> --profile ceiling --seconds 120
```

5. Read the numbers from three places, which should agree:
   * the **HUD** on the device (pps, Mbit/s, mean batch fill, CPU, ring depth);
   * `adb logcat -s nxwarp`, one `NXC-SELFTEST` line per second, greppable;
   * the **blaster's own stdout**, which prints the device's stats report each
     second — so a device run needs no adb at all.

### What the numbers mean

The blaster appends a 16-byte self-test trailer carrying a 32-bit absolute
sequence number. Loss is measured from that, not from the wire's 14-bit
`path_seq`, which wraps every 16384 datagrams — 0.18 s at 90 kpps — and so cannot
by itself tell a 16384-datagram burst loss from none at all.

Three different losses are counted separately, and the distinction is the whole
point of the exercise:

| Counter | Meaning |
|---|---|
| `st_lost` | datagrams that never reached userspace at all |
| `snmp_rcvbuf_err` | `/proc/net/snmp` `Udp: RcvbufErrors` — the kernel dropped them because the socket buffer was full |
| `ringfull` | userspace could not drain the ring fast enough; the receive thread is the bottleneck, not the kernel |

If `snmp_rcvbuf_err` dominates, the fix is a bigger `SO_RCVBUF` or a smaller
burst (PAPER 4.11). If `ringfull` dominates, the receive thread is CPU bound and
the pps ceiling is real. If neither is set but `st_lost` is large, the loss is on
the network or in the driver below the socket.

### Receive-path tuning, and whether the ROM granted it

Every knob PAPER 4.11 prescribes is applied and *verified*, because a ROM may
refuse any of them; the HUD and the stats report say which:

| Knob | Where | Verified by |
|---|---|---|
| `recvmmsg`, batch 64 | `Receiver::thread_main` | mean batch fill on the HUD |
| `SO_RCVBUF` 8 MB, `SO_RCVBUFFORCE` first, halving fallback | `Receiver::open` | granted size logged and shown; Linux reports 2x what it keeps, so the HUD shows the usable half |
| `SCHED_FIFO` prio 10, falling back to `nice -19` | `apply_thread_tuning` | `SCHED FIFO` / `OTHER (REFUSED)` on the HUD |
| affinity to the fastest core | `pick_fastest_cpu` | picked by `cpufreq/cpuinfo_max_freq`, not a hardcoded topology; `CPU n PINNED` on the HUD |
| lock-free ring, no copy from the socket | `nxc_ring.h` | `recvmmsg` iovecs point straight into ring slots |

An unprivileged Android app will normally **not** get `SCHED_FIFO` or
`SO_RCVBUFFORCE`. That is expected, it is reported rather than hidden, and it is
itself a result: it bounds what the real client can assume.

---

## Results

> Device runs pending. The Pixel 7 (serial 27261FDH2002G9, Mali-G710) was
> detached before the first APK finished building — `adb devices` was empty at
> both the start and the end of this work — so **no on-device numbers exist
> yet**. Everything below the host-side row is unmeasured.
>
> Note that a Pixel 7 result would be labelled as such and is *not* a Pico
> result: Tensor G2 is Mali-G710 (Valhall, subgroup 16), and PAPER 3.7 puts Mali
> Valhall on "hybrid only unless a Phase 0 style bench passes". The receive-path
> pps number is CPU-side and transfers reasonably between parts; the decode
> numbers will not.

| Date | Device | Profile | Target pps | Achieved pps | Loss | rcvbuf err | ring full | CPU (rx thread) | Notes |
|---|---|---|---|---|---|---|---|---|---|
| 2026-09-04 | host loopback (x86_64, 127.0.0.1) | 150mbit | 14940 | 12806 | 0 | 0 | 0 | n/a | wire-format and frame-ring validation only, not a pps measurement |
| | Pixel 7 (Mali-G710) | 150mbit | | | | | | | |
| | Pixel 7 (Mali-G710) | 400mbit | | | | | | | |
| | Pixel 7 (Mali-G710) | 1gbit | | | | | | | |
| | Pixel 7 (Mali-G710) | ceiling | | | | | | | |
| | Pico 4 (Adreno 6xx) | 1gbit | | | | | | | |

Host loopback run, for reference (8 s, 150 Mbit/s profile):

```
received datagrams   89664      bad version/caps     0 / 0
placed runs / tiles  89664 / 1219432
bad directory/range  0 / 0      short / duplicate    0 / 0
frames seen/advanced 528 / 528  deadlines fired      3168
tiles concealed/late 1304 / 0   deadline offset      0 us
feedback pkts/bytes  3168 / 228090 (max 93 B, mean 72 B)
```

The mean 72-byte feedback packet is the interesting one: TRANSPORT.md decision D9
predicts "a clean band costs 20 bytes, a band with two loss bursts costs 26"
against a 225-byte raw worst case, and argues the three bitmap encodings are what
restore the paper's 100-byte / 0.4 Mbit/s uplink figure. Measured mean 72 bytes
over three cumulative band records, max 93, confirms it.

---

## The decoder integration point

`src/nxc_decoder.h`. The contract matches PAPER 3.2 so the real decoder is a
drop-in:

```cpp
class Decoder {
  virtual bool create(const DecoderCreateInfo&) = 0;
  virtual void record_pass_a(VkCommandBuffer, const DecodeSubmit&) = 0;  // PAPER 3.2.2
  virtual void record_pass_b(VkCommandBuffer, const DecodeSubmit&) = 0;  // PAPER 3.2.3
};
```

`DecodeSubmit` carries, in the words the paper uses:

* `bitstream` — the host-visible ring the network thread wrote plaintext into
  (PAPER 4.1: "writing plaintext straight into a host-visible ring buffer the
  decoder dispatch reads");
* `tile_runs` — SSBO of `TileRunGpu {tile_index, byte_offset, dir_word, meta}`,
  16 bytes each, 37 KB for a full frame;
* `tile_meta` — the per-slot 4-byte-per-tile metadata of TRANSPORT.md 7.3, the
  same words the CPU wrote;
* `ref_luma[4]` / `ref_chroma[4]` and `current_slot` — the 4-slot reference ring
  of PAPER 4.3 item 1, two planes per slot;
* `output_luma` / `output_chroma` — **2-plane 4:2:0 YCbCr**: `R8_UNORM` at full
  resolution and `R8G8_UNORM` at half, Cb in R and Cr in G. This matches what the
  WiVRn NX client already samples out of MediaCodec (PAPER 3.5); emitting RGBA
  would force a conversion out of the decoder and another back into WiVRn's
  compositor path, and would not match hybrid mode's base layer at all.

The decoder owns its own descriptor set layout, pool, sets and pipelines — the
client hands over buffer and image handles and never reaches inside. The
placeholder demonstrates the pattern in about 200 lines.

`create()` also enforces PAPER 3.2.6's floor: a subgroup smaller than 8 is
rejected at the seam, because that part belongs on the hybrid path (PAPER 3.5),
not on pure compute.

Writing `R8_UNORM` / `R8G8_UNORM` storage images requires
`shaderStorageImageExtendedFormats`. The renderer checks the feature *and* both
per-format bits at init and fails loudly rather than mysteriously. The present
pass reads the planes as *sampled* images, where those formats are mandatory, and
gets the 4:2:0 chroma upsample free from a linear sampler.

## The transport seam

`src/nxc_transport.h` documents exactly what the stub implements (header parse
and validation, directory sum, run homogeneity, duplicate suppression, per-path
loss, and all three feedback bitmap encodings), what it does not (AEAD, RS FEC,
fragment reassembly, multipath), and the three-step swap to `nxvc_transport`. The
commented-out `add_subdirectory` in `CMakeLists.txt` is the fourth step.

The stub's AEAD is the identity function and `kStubAeadIsIdentity` says so at
compile time. It must never be used for a real session.

---

## Known issues and open questions

1. **No device numbers.** The whole Results section is empty. This is the one
   thing that matters and it needs a headset.
2. **AEAD is not measured.** The stub does not decrypt, so the self-test pps is
   the pure receive-path ceiling. PAPER 4.1 argues ARMv8 crypto extensions do
   2–4 GB/s per core against 125 MB/s at 1 Gbit/s, so the margin is large — but
   it is an argument, not a measurement. `AppConfig::touch_payload` reads every
   payload byte to approximate the memory traffic; a real number needs the real
   library.
3. **No FEC.** Parity datagrams are counted and dropped (`parity_dropped`). The
   client does not negotiate `CAP_FEC`, so a sender that honours TRANSPORT.md 2.2
   will not send parity in the first place.
4. **`predicted_display_time` is synthesised.** With no OpenXR there is no
   `xrWaitFrame`, so the band deadline is anchored on
   `first_rx + 2 frame periods`. Every quantity the controller consumes is still
   a client-clock quantity, which is the property PAPER 4.11 actually requires,
   but the anchor is a stand-in. One frame period was tried first and made 80% of
   tiles concealed — all bands of a frame share one display time, so the whole
   frame must land inside it.
5. **No clock offset estimator.** Without ping/pong there is no server-to-client
   offset, so the HUD shows `enc_us`, frame arrival spread and GPU decode time,
   but not the queue and air stages of PAPER 4.9's stacked bar. Cross-clock
   values would be telemetry only in any case (PAPER 4.11).
6. **The blaster is not a timing-accurate sender.** It paces uniformly at a
   target pps; a real server paces a band's datagrams over its encode time
   (PAPER 4.2). Frame-completion timing measured against it is not a latency
   result.
7. **Reference ring images are not allocated.** The frame ring holds the 4-slot
   *metadata* ring (TRANSPORT.md 7.3, 37 KB); the full-size reference images of
   PAPER 4.3 item 1 (about 60 MB) belong to the real decoder, which allocates
   them in `create()`.
8. **`late` accounting is coarse.** Every tile of a run that arrives after its
   band deadline is counted late, which is right, but `band_late` saturates at
   the u16 the feedback record carries.
