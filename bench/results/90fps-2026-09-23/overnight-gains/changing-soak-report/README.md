# Interrupted changing-loss-only soak

This is a retained descriptive capture, not a successful completed soak. The supervisor exited 143 at approximately 900 seconds after upload, with no normal completion marker.

The controller ceiling descends through 500 → 450 → 405 → 364.5 → 328.1 → 295.2 Mbps, while the actual direct planning budget descends from 433.604 to 256.039 Mbit/s and does not recover before interruption. The bundled timeline shows planning budget, codec payload, and fresh FPS together; the numeric controller-ceiling and direct-planning transitions are in `controller_transitions.csv`.

Summary: 444 windows, mean planning budget 304.3 Mbps, mean codec payload 41.20 Mbps, mean fresh FPS 89.55, minimum fresh FPS 79.41, and 5 holes. These values are descriptive only and do not demonstrate quality retention. Derived software delay is not photon latency.

`build_report.py` rebuilds the timeline from bundled numeric CSV only.
