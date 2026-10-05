# Validation record

Source revision: [`7b7ae3600951d90b07466053bbcda45f7dcc9b3a`](https://github.com/nerdrx/wivrn-nx/commit/7b7ae3600951d90b07466053bbcda45f7dcc9b3a), branch `pyrowave-probe`. The four owned source/test files were reviewed, committed and pushed; the remote revision was verified. The source experiment remains default off.

Root executed the published `run.sh` with a private scratch output directory: **exit 0**. It independently rebuilt/reran both actual-selector checks and replay under normal and strict sanitizers. NACK checks:832; replay checks:468; failures:0. Root-generated CSV equals the retained agent CSV. The agent ran `cmake --build build-server -j2 --target wivrn-server`:exit0. This verifies the current disk build only; nothing was installed or launched. `server-build.log` has the local source prefix redacted.

Root inspected the rendered recovery graph and verified the ten CSV rows, two-round bounds, reply totals and readiness against injected fixture losses. The adapter directly calls the production history selector, but does not instantiate the private encoder method or send sockets. Production codec/env gating, timing validation and counter/budget glue were reviewed. Live duplicate traffic, loss feedback and deadline behavior remain unmeasured.

## SHA256

```text
0ad87c95fcc6aa5683fb9abf1d6b9efedadd6eb61d21fa8738a8f5f43030db4f  endpoint_probe.cpp
4066c15039793fc6c7c7c4b59e290dc534c1fe65941da3c6e9d464d6d4ceeb73  results.csv
4066c15039793fc6c7c7c4b59e290dc534c1fe65941da3c6e9d464d6d4ceeb73  results-sanitizer.csv
290ace261b979f731f66a4bc5409f5dda7ac1ecdf6050d53ca0574f744d1b166  run.sh
29c934a1acc0534deb78dff6703b2836cda8013a27d72c7b2318144a5014a05e  plot.py
5e85f9fb0e249b0de0370d1733b82d3277ed6cd2d4ca7f48353b713521a1bec2  nack-normal-run.log
5e85f9fb0e249b0de0370d1733b82d3277ed6cd2d4ca7f48353b713521a1bec2  nack-sanitizer-run.log
74af9ab50f22212e53748b87dba303c1f5f0428fb68ef9264c3bf46690658371  root-endpoint-normal-run.log
74af9ab50f22212e53748b87dba303c1f5f0428fb68ef9264c3bf46690658371  root-endpoint-sanitizer-run.log
da563eb7fc749317f9e8acf032f40455ad3ffc1d1ec2f16e7247f8ad3c213fb4  server-build.log
```
