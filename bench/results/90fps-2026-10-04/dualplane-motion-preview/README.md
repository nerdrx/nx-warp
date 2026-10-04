# Dual-plane synthetic-pan preview

Four small comparisons show dark fur and forest face/hair crops at q2 and q6. Each animated WebP cycles through eight synchronized baseline/candidate pairs; the PNG sheets show every phase together. Each crop is 512×512: forest `(704,128,512,512)`, dark fur `(320,240,512,512)`. No full photo or decoded frame is included.

The selected quality policy uses an error gate of 0.95 at q2 and 0.80 at q6. q2 previews are unchanged; q6 uses the selected 0.80-gate outputs. The 8-phase aligned temporal step RMS rises modestly at q6: dark 4.466→4.671 (+4.6%), forest 2.593→2.626 (+1.3%). q2 is effectively unchanged: dark 6.724→6.734, forest 4.188→4.185. The [compact chart](temporal-rms.svg) plots these values. This is a synthetic one-pixel camera-pan stability diagnostic, not live jitter, headset motion, perceived quality, latency, or runtime proof.

The crops come from the density agent’s final corrected, baseline-sort-matched outputs. Each WebP animates its eight phase pairs; contact sheets provide static inspection:

| Crop | q2, gate 0.95 | q6, gate 0.80 |
|---|---|---|
| Dark fur | [Animation](dark-fur-q2-8phase.webp) · [sheet](dark-fur-q2-8phase.png) | [Animation](dark-fur-q6-8phase.webp) · [sheet](dark-fur-q6-8phase.png) |
| Forest face/hair | [Animation](forest-face-hair-q2-8phase.webp) · [sheet](forest-face-hair-q2-8phase.png) | [Animation](forest-face-hair-q6-8phase.webp) · [sheet](forest-face-hair-q6-8phase.png) |

`manifest.json` and `hashes.csv` record all 64 phase inputs, eight previews, and the chart. `inputs.json` preserves read-only input paths and expected hashes; `build_preview.py` documents the bounded rendering step.
