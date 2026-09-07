# ATLAS admission diagnostic

This capture records the encoder's per-eye first-rejection counters while
aged entries were allowed. It is diagnostic evidence for the observed forced
missing gate; it does not establish a feedback bug or causality. The raw
pace45 scene, server, measure, startup, screenshots, manifest, configuration,
experiment patch, and provenance are retained here.

Parse the server diagnostic with:

```sh
python3 summarize.py live-admission-q40-pace45-server.log \
  --out admission-summary.json
```

The parser reports invalid, unconfirmed, aged, refresh, admitted, missing,
no-ref, and displacement counts separately for left and right eyes. In steady
windows the observed pattern was approximately 8,670 missing and 8,670
admitted entries per 60 frames per eye. This is an observation of the capture,
not proof of why the gate was forced.
