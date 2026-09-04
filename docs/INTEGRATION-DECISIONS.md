# Integration decisions

Answers to the open questions raised by the WiVRn NX integration spike in
[INTEGRATION.md](INTEGRATION.md). These are decisions, not proposals; each one
that changes the design gets an ADR under `adr/`.

## 1. Colour space: the codec takes what the source has

WiVRn's compositor hands the Linux encoder a 4:2:0 YCbCr two-plane image that is
already foveated. The Windows helper hands RGBA from SteamVR. The client already
samples 4:2:0 YCbCr from its hardware decoder today.

Decision: the stream header carries a `color_space` field.

| Value | Meaning | Source |
|---|---|---|
| 0 | YCoCg-R, computed by encoder pass E0 from RGB | Windows helper, RGB compositors |
| 1 | YCbCr passthrough, planes coded as delivered, range flag | WiVRn Linux server |

The transform, quantiser and entropy stages are identical in both modes; only the
colour conversion stage is present or absent. This supersedes the paper's
assumption in section 3.6 that the encoder imports an RGB render target.

## 2. Capture format: native 4:2:0, no upsampling in the tap

The capture patch dumps the encoder's own NV12 or P010 planes. Upsampling in the
tap would invent precision. The quality harness handles 4:2:0 natively.

## 3. Reference and output format on the client: 4:2:0 for the 4:2:0 path

Four RGBA8 slots per eye at 2048 squared would cost 134 MB on the Pico 4. With
YCbCr passthrough the references are stored as two-plane 4:2:0, which is 50 MB for
the same ring, and the decoder writes the exact image format the client's
reprojection shader already samples. Zero shader changes for the plain output
path. RGBA8 and RGB10A2 display-format references remain for the YCoCg-R and
4:4:4 paths. This supersedes paper section 1.3's "references in display format"
for the 4:2:0 path and gets an ADR.

## 4. Retransmission stays alongside reference tracking

A NACK that lands inside the presentation deadline beats concealment. Both are
kept: the transport's `shard_history` retransmit path serves tiles that can
still make the deadline, and per-tile reference tracking handles everything
that cannot. The `idr_handler` ladder is retired.

## 5. FEC ceiling

Blended overhead target is 14.5 percent per paper 4.4. Hard cap 20 percent
overall, with the adaptive controller allowed up to parity 4/2/1 for classes
A/B/C only under measured loss above 3 percent. The existing WiVRn adaptive FEC
controller is replaced by the codec's class-aware allocation, not run alongside.

## 6. Encoder watchdog

The NX Warp encoder calls `set_eligible(false)` on the watchdog until failover
can renegotiate the codec with the client. A silent swap to VAAPI mid-session
with an NX Warp decoder on the other end is a hard failure.

## 7. Windows synchronisation

The keyed mutex mandated by the frozen IPC contract (v3) is the shipping design.
The shared-fence timeline path from paper 3.8 is an optimisation for a later
contract version, not a requirement.

## 8. Partial-frame retirement

The codec's frame ring owns the policy. The client transport delivers late tiles
into the ring until the slot is reused; the ring decides what is presentable and
what becomes reference. WiVRn's `frame_window<6,3>` skew bound applies only to
the hardware-codec path.

## 9. Pose at present time

`present_image` gains the `view_info_t` argument so the encoder has the pose when
the frame arrives, not one call later. About 25 lines across the encoder base
class and its implementations.

## 10. Queue mutex

Sixteen row-group submits per frame serialising against Monado's queue mutex is
accepted for Phase 3 and measured. If it shows in the timeline, the encoder moves
to a dedicated compute queue where the device exposes one.
