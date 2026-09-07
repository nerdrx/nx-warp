# GPU atlas table handoff

The new `nxvc_vk_decoder_atlas_table_buffer` accessor exposes the existing normative
64-byte-per-tile GPU table without a CPU readback. It does not synchronize or own a
snapshot. Consumers must finish reading before subsequent decode/patch work mutates
it; WiVRn's experimental integration copies it into the same frame pool as its images.
The table allocation now explicitly includes transfer-source usage.

Validation on 2026-09-07: host RADV with synchronization validation completed the
four-arm R8/R16 full/dirty comparison, including view recreation, without validation
errors. Pico completed the same comparison with `NXVC_TEST_ATLAS_VIEW_NODIR=1`
(the known directional-intra driver issue remains separate). Both logs are retained.
The test also checks that every decoded frame exposes a non-null GPU table with the
expected byte size. These checks establish API/fixture consistency, not a live FPS gain.

The custom WiVRn NX atlas renderer is still an isolated integration experiment.
