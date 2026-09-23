# NX safety synthetic detail-loss timing

This public artifact summarizes the steady portion of the private
`photo-safety-crowd-loss1` run. The runner dropped detail for 30 of every 180
source frames. Events in the first 10 seconds after the RGBA8 upload marker
are excluded so startup does not inflate the cycle count. The parser removes
device log prefixes and source timestamps; it retains only source-frame indices,
phase modulo 180, and primary-hold durations.

The steady window contains 27 fallback events and 27 detail-return events.
Fallback holds were about 22.3 ms. Detail holds were usually about 345.3 ms,
with a few 356.4 ms holds. The detail-return phase counts are recorded in
`summary.json`; 20 returns are mod 90 or 91 and 7 are other phases. These are
logged frame phases and hold durations only; they do not establish restoration
latency. The ~345 ms holds include the forced 333 ms outage interval.

This is headset log timing from a synthetic loss run. It is not proof of photon
latency, total radio outage duration, or uninterrupted visual restoration.

Regenerate the JSON and graph from the bundled sanitized CSV with:

```text
python3 plot.py
```

For private-log verification and to rebuild `events.csv`, pass the private
client log as the optional argument.
