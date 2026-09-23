# Live predictor repeat report

This sanitized report contains numeric summaries from four complete live repeats. The implementation label is **old scalar implementation; pre-pointer-fix**. Case A is cache off (repeats 1 and 4); case B is cache on (repeats 2 and 3). ABBA means are the average of those two run means per case.

`codec_wire_mbps` is the server's actual codec payload rate and excludes link overhead. `server_encode_ms`, `client_decode_ms`, `fresh_fps`, and `derived_software_delay_ms` are included in the CSVs. The derived delay is software-derived and must not be read as photon latency.

The p95 column is explicitly `p95_window`: a percentile over 2-second summary windows, not a per-frame percentile.

The byte win is about 10%, but the repeated runs show a fresh-frame regression and holes in the cache-on case. This result does not promote the setting as a default.

The four repeats are bounded live runs; the report contains no raw logs, private paths, identifiers, or images.

Rebuild the graph from the included numeric CSVs with `python3 build_report.py`.
