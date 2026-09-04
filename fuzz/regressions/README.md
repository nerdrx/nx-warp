# fuzz/regressions/

Permanent reproducers. `<target>/` is replayed by `ctest -R '^fuzz\.'` and by
both CI workflows, so a fix stays fixed.

`<target>/open/` holds reproducers for findings in
[../FINDINGS.md](../FINDINGS.md) that are **not fixed yet**. Those inputs still
abort, so they are skipped by default and the CTest entry stays green while the
component owner works. Replay them explicitly:

```sh
# every open finding at once
build/bin/nxvc_decode_fuzz_replay --include-open fuzz/regressions/nxvc_decode_fuzz

# one input, by name (an explicitly named file is always run)
build/bin/nxvc_decode_fuzz_replay \
    fuzz/regressions/nxvc_decode_fuzz/open/F3-idct8-1d-signed-overflow-178.bin
```

When a finding is fixed, move its file up one directory. That is the whole
ceremony: it then runs on every push like the rest.

Naming: `F<n>-<what-it-hits>[-<size>].bin`, where `F<n>` is the FINDINGS.md
entry.
