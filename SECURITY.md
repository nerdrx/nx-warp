# Security policy

## Status of this project

NX Warp is pre-release research code. There is no released version, no deployment, and no user base to
protect yet. See [ROADMAP.md](ROADMAP.md) for what actually exists.

That said, the codec parses untrusted network input by design, and the reference decoder in `ref/` is
intended to become the specification other implementations are tested against. Bugs found now are
cheaper than bugs found later, and reports are welcome at any stage.

## Reporting a vulnerability

**Please do not open a public issue for a security problem.**

Report privately through GitHub's private vulnerability reporting on
[nerdrx/nx-warp](https://github.com/nerdrx/nx-warp): the Security tab, "Report a vulnerability". That
creates a private advisory visible only to the maintainer.

Please include:

- The component (`ref/`, `transport/`, `vk/`, `android/`, and so on) and the commit you tested.
- What an attacker can do, not only what the code does wrong.
- A reproducer: a bitstream, a datagram capture, a fuzzer input, or a short program. For decoder
  issues, a `.nxv` file plus the command line that crashes `nxv-dec` is ideal.
- Your assessment of severity, and whether you intend to disclose publicly and when.

### What to expect

This is a single-maintainer project, so response times are best effort rather than contractual:

| Stage | Target |
|---|---|
| Acknowledgement of the report | within 7 days |
| Initial assessment (is it a vulnerability, and how bad) | within 14 days |
| Fix or a documented mitigation for accepted reports | best effort, discussed in the advisory |
| Public disclosure | coordinated with the reporter, default 90 days after acknowledgement |

If a report is not a security issue but is a real bug, it will be moved to a public issue with the
reporter's agreement.

Credit is given in the advisory and in [CHANGELOG.md](CHANGELOG.md) unless the reporter prefers
otherwise.

## Scope

### In scope

- **Memory safety in the reference codec** (`ref/`): out-of-bounds reads or writes, integer overflow
  reaching an allocation or an index, infinite loops, unbounded allocation, or any crash from a
  malformed, truncated or hostile `.nxv` stream. The stated properties are "never reads out of bounds"
  and "always emits a frame" (paper 3.9); a violation of either is a bug worth reporting.
- **Memory safety in the receive path** (`transport/`, `android/`): anything reachable from a datagram
  before or after authentication.
- **Cryptographic transport flaws** (`transport/`): nonce reuse across `path_seq` wrap or session
  reuse, replay window defects, missing or incorrect associated-data coverage of the header, key
  material leaking into logs or telemetry, plaintext released before tag verification. See
  [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md) section 7 for the implementation obligations these come
  from.
- **Feedback handling** (`transport/shadow`, `rc/`): anything where forged or implausible feedback
  causes worse than the bounded degradation the design accepts.
- **GPU decoder safety** (`vk/`): out-of-bounds buffer or image access in a normative shader, or a
  shader that can be made to hang. Every load is meant to be bounds-clamped in the shader, because
  `robustBufferAccess` behaviour differs across vendors and the codec does not depend on it
  (paper 3.7).
- **Build and supply chain**: a dependency or CI configuration that lets an attacker influence what is
  built.

### Out of scope

- **Session establishment, authentication and key agreement.** These belong to the host application
  (WiVRn NX), not to this codec. Report those to that project.
- **Endpoint compromise.** A compromised OS on either end reads the framebuffer directly.
- **Traffic analysis.** Payload sizes and timing are content-dependent by construction and padding
  them would cost exactly the bits the codec exists to save. This is a documented, accepted exposure;
  see [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md) section 6.
- **The cleartext datagram header.** It is authenticated but readable, deliberately, because the
  receiver needs it before decryption and FEC repair works across datagrams. Also documented.
- **Content protection and DRM.** This codec does not attempt to protect content from the user of the
  headset and has no protected output path.
- **Denial of service by network congestion** from an attacker who can saturate the air. No
  codec-level defence changes that.
- **Vendor components**: MediaCodec, GPU drivers, the OpenXR runtime. Report those upstream. A bug in
  how this project *uses* them is in scope.
- **Findings from automated scanners with no demonstrated impact.**

## Supported versions

None yet. There has been no release. When releases begin, only the latest release will receive
security fixes unless a specific version is stated otherwise here.

| Version | Supported |
|---|---|
| `main` (pre-release) | best effort |

## Hardening that already exists by design

These are design commitments, not claims that they are implemented and audited. They are listed so a
reviewer knows what the intended invariants are and can report a violation as a bug:

- Integer-only normative path: no float, no int64, no integer division or modulo, and rounding shifts
  written in a single defined form (paper 3.7).
- Coefficient clamping ranges are normative, so overflow cannot differ by vendor.
- A decoder refuses a stream with an unknown mandatory tool bit rather than guessing (paper 1.2).
- Per-tile byte caps (1372 bytes) and a fixed tile grid, so there is no content-driven unbounded loop
  or allocation in the decoder.
- Fuzzing is a release gate: libFuzzer with a structure-aware mutator on `ref/`, and the same corpus
  against the GPU decoder under the Khronos validation layers with GPU-assisted validation on
  lavapipe, where a timeout counts as a bug (paper 3.9).

## References

- [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md), the full threat model
- [docs/PAPER.md](docs/PAPER.md) sections 3.7, 3.9, 4.1
- [CONTRIBUTING.md](CONTRIBUTING.md)
