# Compositor retirement transition gate

This CPU-only gate tests a candidate state machine for a separate compositor-submit fence. It models lifecycle transitions only; fake `result` values stand in for Vulkan-Hpp results. It does not call Vulkan, inspect command buffers, or establish real resource ownership.

The source context at checkout HEAD `09e7951a6e3273e35dccfacefebdd27303211866` submits compositor work with `queue.submit2` and signals a timeline semaphore (`server/compositor/compositor.cpp:1151-1173`). That semaphore is waited at `:1220-1248`; timeout sets `motion_unsafe` and clears `motion_pending`, while success permits query-result retrieval and motion-field send. Query statistics are recorded only when query retrieval succeeds. The gate keeps those conditions independent: compute-timeline success cannot retire a compositor-submit generation. It models a mirror reader as a separate consumer with its own completion token; even compositor-fence success does not allow image reuse while that reader remains active. Compute timeout retains the current no-query/no-motion path. A compute-wait error is modeled as aborting before query/motion work; it does not model or claim a specific Vulkan-Hpp exception-state update.

The modeled retirement fence publishes a fresh generation and `pending=true` only after reset and submit success. Reset/submit failures do not advance or complete a generation. A pending generation rejects reacquisition; timeout/error preserve it; only a successful status for that exact generation retires it. A stale completion for an earlier generation cannot clear a newer pending generation. The assumed failure semantics are explicit: failed reset/submit does not publish a candidate submission. The compositor-submit fence by itself cannot retire downstream image consumers. Real integration must track each consumer (for example a mirror queue) independently and allow reuse only after all relevant completions; device-loss handling remains out of scope.

Cases cover initial state, reset/submit failure, duplicate acquire rejection while pending, healthy compute signal with mirror-tail pending, compositor fence completing before mirror completion, timeout/retry, compute timeout/error abort, query error, late stale completion and generation reuse. The original runs before this correction remain in `results/final/`. After correction, normal and halt-on-error ASan/UBSan executions each pass 37 checks in `results/revised/`. Each executable is bounded by `timeout 30`; output and updated source hash are retained in those directories.

Run from this directory:

```sh
rtk proxy ./run-check.sh results/revised
```

No production source, runtime, GPU, or device state was changed.

The extra mirror-reader case is an illustrative shared-resource assumption. It is not a reproduction of the actual mirror buffer layout or an implemented native-image acquire rule. The production patch below protects compositor recording resources only.
