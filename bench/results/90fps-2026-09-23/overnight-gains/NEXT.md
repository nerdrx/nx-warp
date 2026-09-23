# Next bounded experiments

The verified compression work should remain independent from controller changes.
The current loss-only AIMD diagnostic prevents receive-span-only downward cuts,
but retains the original healthy threshold. On this Pico, a roughly one-refresh
receive span can prevent an upward recovery even after losses stop. Radio-aware
cuts also remain active: the longer capture logged PHY-rate-based cuts while
RSSI was roughly stable. A reported PHY-rate fall is not by itself proof that
the much smaller compressed payload no longer fits; that relationship also
needs measurement. The diagnostic is not a finished automatic controller.

The next useful change is to distinguish the **representation quality budget**
from **actual compressed transport demand**, then test recovery using both fresh
frame delivery and real queue/loss evidence. Do not replace either with a scaled
bandwidth guess: the archived nominal-BBR experiment still collapsed quality.

Before changing the law, capture per-frame sender duration, receiver arrival
span, actual payload, queue age and loss together. Current receive timestamps
are userspace handler times, so batching and scheduling can contribute to the
span. No next-frame completion deferral was found in the code audit. Kernel
receive timestamps could distinguish those causes, but would need careful clock
conversion and their own measured overhead.

A successful controller experiment must preserve the requested detail on a
healthy link, lower actual wire demand during a controlled impairment, and then
recover without renewed loss or stale-frame accumulation. Include a real game
and wearer assessment before making it the normal profile. Keep the current
lossless predictor separately selectable so a controller regression cannot hide
its established byte savings.
