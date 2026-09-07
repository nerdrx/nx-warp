# ATLAS all-skip upload experiment

This experiment tested suppressing staging and transfer copies for ATLAS
frames with no coded tiles. The optimized arm used the default decoder; the
control set `NXVC_VKD_ATLAS_FORCE_UPLOADS=1`. Both used the same final Android
CLI, still ATLAS fixture, Pico device, R8 atlas view, `--no-out --stats`, and
four alternating runs plus one reverse-order pair. Frame 0 was excluded;
each arm has 93 warm frames across three runs. The parser excludes zero or
unavailable GPU samples and computes p95 by linear interpolation.

Reproduce from the raw logs with:

```sh
python3 summarize.py
```

The optimized arm had GPU p50/p95 of 0.962/2.422 ms and wall p50/p95 of
1.362/3.838 ms. Forced uploads had GPU 0.990/3.332 ms and wall 1.224/4.056
ms. The small GPU gain was outweighed by the 0.138 ms wall median regression,
so this optimization was rejected. Raw commands, logs, hashes, and the
isolated experiment patch are retained alongside this report.
