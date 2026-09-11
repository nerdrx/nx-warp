# Headset focus gate for visual trials

A running client and advancing synthetic scene did not establish a valid visual test: the failed source-rate recording showed Pico's dark-environment tracking dialog. The harness now records headset focus separately from execution completion.

The [log-only checker](../../../tools/pico_focus_check.py) tracks session transitions and classifies each render-statistics window as focused, unfocused or unknown. Its conservative gate passes only if all observed render windows are focused and the final recorded state remains focused. Missing logs fail the gate.

| Existing recording | Render windows | Focused | Gate |
|---|---:|---:|---|
| Dark-environment source-rate capture | 11 | 0 | Reject |
| Earlier four-source recording showing the scene | 11 | 11 | Pass |

**Passing does not verify the pixels.** Output always sets `visual_verified=false`. Tracking quality, scene correctness, capture synchronization and physical display cadence still require separate checks. Startup focus changes can conservatively reject an otherwise usable later capture.

The local capture helper adds this metadata to every future status file. With `NX_REQUIRE_FOCUSED=1`, failure marks the run incomplete and returns nonzero. The source-rate visual wrapper enables this requirement and now checks the child process exit code. Ordinary timing runs may still complete unfocused, but their status reports that limitation.

Five regression tests pass: focused-state reporting without visual proof, visible-but-unfocused rejection, absent telemetry, focus loss after the last render window, and recovery that does not erase an earlier unfocused window. Python syntax checks pass for both updated helpers. Existing recordings were re-evaluated without running the headset or changing its settings.

Run `python3 -m unittest discover -s bench/tools -p test_pico_focus_check.py` from the repository root. Included launch scripts retain local scratch paths. Historical archived scripts are not silently rewritten.
