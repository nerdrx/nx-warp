# Full-resolution native atlas evidence

Commit `944b0cf0` fixes atlas grid normalization and dynamic R8 dimensions.
The resulting APK is `1a484ee3a88c57ac74402ba8d7a3a127b2d825c8e03e9b46bb473d560b6aced3`;
the server is `a7b47b3e164eaa1578774526598911546b00ae83b55ba7a33f874e2504040d82`.
The codec source is 2176x2176 per eye and the display output is 2160x2160 per eye.

Canonical fixed/auto active-window medians were decoder GPU 58.6/13.4 ms,
decoder wall 64.5/17.0 ms, renderer GPU 14.76/35.25 ms, and copy 0.01/0.01
ms. Fixed reported all-INTRA 2312 frames; AUTO reported many skips, consistent
with feedback starvation (an interpretation, not a causal proof). The fixed
capture shows the full checkerboard and centered cube scene with real edges and
block artifacts. This is full-field evidence, but not a 240 FPS claim.

The earlier 0.40 and AUTO 0.50 timing runs remain archived with their cropped
field-of-view caveat. The 0.40 output is no longer the default; native full
resolution is the current priority. Raw captures, configuration files, patch,
and screenshots are retained here with checksums.
