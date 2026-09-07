# Preliminary R8 dirty-view capture

This preliminary matched pair compares full and dirty R8 atlas views at fixed QP 40. Both active-window medians report 45 new sources/s. Decoder GPU medians are 9.8 ms (full) and 8.9 ms (dirty); boundary-excluded wall counts are 29.18/s and 35.82/s, with report gaps up to 5.995 and 4.456 s. A 25 s reverse full repeat measured 8.8 ms versus dirty's 8.9 ms, with 45 new sources/s in both arms; the shorter repeat and session gaps show no repeatable gain.

The temporary client setting used for the dirty experiment has been removed; the full path remains the default.

The baseline APK is `5a095bbc730a51f52522f4e3f2ae103169ace65f57006f3dc4fa7614bd7721ac`; the dirty candidate APK is `c35f3bcca986b56e8d41bacdbd4ccbc9e80878d1d8b6dca18535594cb601bb78`. Device serials are omitted from the retained manifests. Screenshots are capture artifacts; the displayed path remains the current R8 scope.

Recompute with `python3 summarize.py`; it imports the parent parser and preserves the original report timestamps in the filtered logs. The manifests record requested and actual capture durations without publishing the device serial.
