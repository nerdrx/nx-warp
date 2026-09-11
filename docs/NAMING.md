# NXVC naming

**NX Warp** is the overall project. **NXVC** names its video system, with the active compression path stated explicitly.

| Name | Meaning |
|---|---|
| Native NXVC | The custom NX compression and reconstruction path. |
| NXVC Hybrid | HEVC image compression plus NX motion prediction and reprojection. |

Preferred description: **NXVC Hybrid — HEVC compression with NX motion prediction and reprojection.**

In Hybrid mode, HEVC encodes and decodes the source images. NX supplies the additional motion processing and presentation logic. This is not a new HEVC-independent compressed image format. Protocol identifiers such as `h265` remain accurate and unchanged.

Reports must identify the mode, source resolution, fresh-source cadence, viewer cadence and enabled motion features. Hardware decoding, prediction coverage and physical motion-to-photon latency are separate measurements. The name does not establish a quality, performance or latency advantage.

Historical native-codec results remain native results; they must not be relabelled as evidence for the Hybrid path. Existing experiment names and raw logs retain their original terminology for reproducibility.
