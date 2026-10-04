# Reuse fixed recovery-blob metadata storage

`encode_blob` and `xor_blob_into` now clear and reuse one `serialization_packet` per function and thread, following the existing socket send path. Current view/timing fields have bounded sizes. The shard payload is always serialized as a borrowed span, including short or empty payloads; only its encoded length joins the metadata buffer. No payload-sized storage is retained. Capacities are retained until that thread exits.

This removes repeated metadata-vector growth. Blob output, parity copying, recovered owned data and strict parsing are unchanged. Reuse is private to each function and thread; serializers for these fixed shard fields have no recursive FEC call. No pointer or span into this storage is returned: `encode_blob` copies into its caller-owned output, and span XOR completes before returning. TLS spans may hold stale non-owning pointers after a call, as in the existing socket send cache; the next call clears the span list before conversion, and destruction never dereferences those pointers.

Validation covers 800 changing-shape calls across four threads (payload sizes 0,7,31,32,1400; optional view/timing fields) against an independent serializer. The complete FEC suite passes 4296 checks normally and with ASan/UBSan and TSan. Valid wire bytes, malformed-input rejection, send-encryption snapshot ownership and recovery checks remain covered. Server/OpenXR and Android native-library builds pass; no APK, property or live session change.

A matched desktop CPU 3-pinned O2 ABBA probe compares source cbe970d4 against this change: recovery has 360 rows of 2500-call loop means, and blob encoding 320 rows of 10000-call loop means. Median condition/block recovery mean falls 1.635→1.325 microseconds; allocation calls 65→2 and requested bytes 4727→2962. Warm encoding has zero metadata allocations versus median 12 before; median loop means 85→40.5 ns. These are component results on a desktop CPU with unlocked clocks, not packet-to-display latency or a Pico performance result.

## Evidence and reproduction

The CSVs contain condition/block loop means, not individual-call latency samples. Percentiles across them must not be presented as per-call tails. Encoding preserves an already allocated caller output buffer, matching `history_blob` reuse in `SendData`; its warm result does not eliminate output/parity/history copies. Recovery retains parity copying, owned decode output and strict validation. This supplements the earlier direct-span XOR change rather than comparing against the original full-blob reconstruction again.

[Recovery rows](recovery.csv) · [Encoder rows](encode.csv) · [Wire baseline](wire-baseline.txt) · [Wire candidate](wire-candidate.txt)

To rebuild, compile each probe twice with C++23 and O2, changing only the first common-header include directory between the baseline revision cbe970d4 and candidate 076d55da. Also include the matching generated common headers, external OpenXR headers and Boost PFR include root; link the project's common/smp.cpp and libcrypto. `run.py` uses adjacent encode/recovery-baseline/candidate executables, pins CPU 3 and runs five matched ABBA blocks. `plot.py` regenerates the figure.

![Warm metadata allocation and component time](comparison.png)
