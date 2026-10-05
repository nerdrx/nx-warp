# Concurrency validation record

Source:7b7ae3600951d90b07466053bbcda45f7dcc9b3a. The test calls the production history helper and actual FEC encoder/decoder; production source is unchanged.

Root reviewed the full payload identity oracle and bounds, then executed the published run.sh independently. Its normal, ASan/UBSan and TSan builds/runs all completed within45seconds each and the script exited0. Root capture:normal1,487,037 hits; ASan/UBSan52,429; TSan21,091. Every run reports0 identity failures, with no sanitizer/race diagnostics. These hit counts vary with scheduling and must not be interpreted as performance measurements. They are not exhaustive interleaving or live network proof.

The agent capture is retained separately in normal-run.log/san-run.log/tsan-run.log; root captures are root-*.log. Program success also checks nonzero hits, published20000frames, bounded held entries and expected allocated ring bytes.

## SHA256

```text
cedf8ee716c5c965bf6a69e4f574fb9472e3ce72e5e64b2e1dc23cba29b6ad6e  endpoint_concurrency.cpp
ce2169f054d31b475eae0c3114b8d687dfd0d5ee349f651ebb84c8a758f76481  run.sh
23c8ee4c77e8e72a2c99c89d86579be2b3c28ff7cc62d637841bdf82e761a7ee  normal-run.log
416a35ee9bbb16954404a25ce061c7a78bcb4f02c73d734fe2bf6159b19c3430  san-run.log
90cd065abc719387169ce2aea9e64471760d0c50e0f599d3722781aa9f480147  tsan-run.log
cae0df746b2f8c5c0b2ece19841a9ab1b7e0577d809bd8b8c23ac5ec5d9cd9f9  root-normal-run.log
06b4c60c0b6148a1ce868699a7cad7d2bf5e6dd9dfaa9132c374da10c5bd4ced  root-san-run.log
e3c9e4806275698af041cacf3e3b57edca4c126c7227a8cdd0033565859b327e  root-tsan-run.log
4ab418b9b23e3f5f92e59f481273e7ba17c12c8df27ec4c323a39dd9f8f7eb80  source:server/encoder/shard_history.h
7b15a9c41ba83cbc0a7841776fa882a9d7944eba21a3de9aba716de6486712be  source:common/fec.h
```
