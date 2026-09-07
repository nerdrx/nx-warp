# Cubic colour approximation experiment

This short, opt-in experiment tested a cubic approximation to the existing
sRGB transfer formula in the live output-scale-0.40 path. Probe APK
`96beeb4a4eb4fc160e9f1b4e93b29236b5dc0ff0309cdd18522b80c185a3b360`; control
`0582f10bd6a4c45da161981381122ad539b4deab212a1fdb1595ce8db23e8e99`; server
`a7b47b3e164eaa1578774526598911546b00ae83b55ba7a33f874e2504040d82`.

Canonical baseline/probe/reverse medians were renderer GPU 2.6/2.4/2.5 ms,
decoder wall 2.2/2.2/2.2 ms, decoder GPU 1.2/1.3/1.2 ms, copy
0.29/0.30/0.28 ms, active sources 88/87/88 per second, and reported
wall-span rates 66.3855/62.2855/59.9792 per second. These active-window
measurements have session gaps and shared-load uncertainty; the marginal stage
delta gave no wall-time benefit. The shader approximation was therefore not
retained.

The tested polynomial was `c*(c*(.3053040462611011*c+.6821741186702331)+.012521835068665776)`.
`error.py` reproduces its 100001-sample error against the sRGB formula:
maximum absolute linear error `0.0016708044094288794`, RMS
`0.0008812373849124702`. Raw logs, manifests, screenshots, patch, canonical
parser outputs, and checksums are retained here.
