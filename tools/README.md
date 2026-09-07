## Live capture preflight

`validate-live-probe.py` is a fail-closed preflight for recorded live
captures. Callers provide the candidate binary and expected SHA256, startup/
scene/server logs, required runtime banners, negotiated dimensions, and a
nonempty stream-0 timing CSV. It rejects missing binaries, hash mismatches,
missing banners or dimensions, and timing files without a stream-0
`receive_begin` record. The checker emits paths supplied by the caller; keep
private logs and manifests outside published evidence. Synthetic coverage is
in `tests/python/test_validate_live_probe.py`.

This checker validates only the files supplied to it. It cannot independently
establish live-process or session identity, scene equivalence, or bitrate
comparability; each published capture still needs recorded executable/config
provenance and screenshot verification.
