# Live WiVRn NX new-source evidence

Source logs: `../nx-scratch/240fps-20260907/live-new-source-logcat.txt` and
`live-new-source-direct-logcat.txt`. Candidate APK SHA256 is
`b44c605b54200f272be792e4bd798278750afe2139c5989bfb19c385ab4ade4e`, matching
the previous certificate. Desktop and Android builds passed.

The selected direct capture covers every two-second render window from
17:28:16.637 through 17:29:09.644. The first three windows are startup and are
excluded from the stable summary. Render windows (iterations/s, new-source)
are: `84.2/66, 85.4/119, 86.5/128, 86.6/134, 85.0/130, 86.0/133,
85.4/130, 87.1/128, 84.6/131, 86.0/135, 86.4/129, 87.3/126, 87.7/123,
86.4/130, 88.3/129, 86.8/132, 87.7/129, 87.5/127, 85.8/129, 86.0/136`.
Thus the stable windows (17:28:25 onward) span 84.6–88.3 render/s and
61.5–68.0 new-source/s (two-second high-water counts divided by two).

The counter counts high-water source IDs at projection additions for stream 0;
it is not a physical-FPS measurement. Decoder summaries over the same capture
report 139–169 frames per two seconds, 10.5–10.7 ms GPU and 12.9–13.1 ms wall
on the steady full-frame windows, with holes 0 and refused 0. Lower-load
windows are present in the log and are retained above rather than folded into
the stable range.

This live session is atlas-mode 0, so the atlas-specific clear, pipeline-demand,
and dirty-view optimizations have no live gain claim. `screencap` was again
all-black and is not repository visual evidence. The mDNS connect helper
falsely retried an active session, causing `xrEndSession` followed by finalizer
SIGSEGV; this is under investigation and gives no conformance claim.
