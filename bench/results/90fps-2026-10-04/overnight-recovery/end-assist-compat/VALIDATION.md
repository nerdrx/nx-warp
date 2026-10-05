# Root validation

2026-10-05 03:41 UTC: the published wrapper was independently rebuilt and executed against exact source7b7ae360 and archived clientc514841f. Exit0. Both normal and strict ASan/UBSan report320 matching payload bytes and complete frame7331 after terminal arrival. Root additionally compares all five current FEC blobs through the archived decoder with the archived parsing of current serialized packets, including payload length and metadata presence. Existing identity and terminal timing assertions remain.

`root-normal.log`, `root-san.log` and `root-provenance.txt` preserve this check. Temporary headers, binaries and synthetic packet artifact were removed by the wrapper. No production source, runtime or device changes. The nonempty-foveation fixture and incomplete session/encoder coverage remain as documented in README.

```
3c6113ccb5176b2389d68214bb21d2fafe25d4ccfd60d35e2b89c7990b583c12  run.sh
94f9ee6748f64bc65eac6a24bf956a9111f24bcf0faa730da262898726b09047  make_server_frame.cpp
8e523d4b125ed2dd7499c26a5504085d06d59c0acbdbddb94f16d5ed91448188  old_client_replay.cpp
34b245dab3bdb02ceb2e3cb570fb2a11d88b37b2545f1f27e118dc6ef94fe6aa  root-normal.log
34b245dab3bdb02ceb2e3cb570fb2a11d88b37b2545f1f27e118dc6ef94fe6aa  root-san.log
926920c4887554095d09bdb14c8398033bec89307d6bc0a6c18d041e10611218  root-provenance.txt
```
