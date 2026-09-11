# Preserve four distinct frames when IDs skip

**Host regression tests and Android build pass. Not installed on the Pico yet; no live jitter or latency gain claimed.**

The opt-in four-frame buffer previously inserted each decoded frame into `frame_id % 4`. This preserves four frames only for suitable ID sequences. A stride of two alternates between two slots; a stride of four repeatedly overwrites one. Missing frames or intentional source strides can therefore undermine the retention experiment even without memory pressure.

| Consecutive decoded ID stride | Slots used by old modulo-4 indexing | New retained distinct frames, once filled |
|---|---:|---:|
| 1 | 4 | 4 |
| 2 | 2 | 4 |
| 3 | 4 | 4 |
| 4 | 1 | 4 |
| 8 | 1 | 4 |

The new opt-in path fills empty slots, replaces duplicate IDs in place and otherwise evicts the lowest retained frame ID only when the incoming ID is newer. Late arrivals older than the retained set are discarded through the existing feedback/release path. Lookup scans the four slots for the exact ID. It retains the newest IDs, not the most recent arrival order, following the existing monotonic frame-ID convention. Decoder/session reset continues to own clearing the buffer.

The original three-slot mode keeps its existing indexing. Exact stereo matching, source selection, image/pose warp fractions and buffer capacity are unchanged. Insertions remain under the existing frame lock, and displaced image handles retain their normal release path. The scan is bounded to four entries.

## Checks

The standalone C++ regression uses the production slot-selection helper. It checks newest-set retention after every insertion for strides 1, 2, 3, 4 and 8, duplicate lookup, rejection of stale arrivals, insertion of an out-of-order but still useful frame, and empty capacity. Compiled with C++20, `-Wall -Wextra -Werror`; all checks pass. Android release assembly also succeeds.

This establishes the slot-selection behavior, not GPU image lifetime under load, full stereo integration or optical motion quality. Headset visual testing remains blocked by the runtime's dark-environment tracking warning, so the installed client and live settings were preserved. The built APK must undergo a short functional test before deployment is considered validated.

Reproduction in the WiVRn checkout: `g++ -std=c++20 -Wall -Wextra -Werror -Iclient tests/retained_frame_slot_test.cpp -o /tmp/retained-slot-test`, then run that executable. Included helper/test snapshots and logs record the offline result.
