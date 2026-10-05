# Raw feedback-span CSV diagnostic validation

Source checkout: `/run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe`, HEAD `412a2bfe9ef54333fdc35cf9919c6443fa409909`.

The only source change is `server/driver/wivrn_session.cpp`. When the existing `WIVRN_DUMP_TIMINGS` CSV is open, `on_frame_sent` emits a `frame_bytes` row with the existing event/frame/server-monotonic-time/stream columns and byte count. The feedback handler emits `feedback_spans` before its clock-offset early return, carrying the same feedback's raw send-begin, send-end, first-receive, last-receive, and sent-to-decoder timestamps. Those five values stay in their original headset timestamp domain; the row time is a separate OS-monotonic diagnostic timestamp.

The environment-disabled path performs no added timestamp read or string formatting. No runtime was started and no environment setting was changed.

Build command: `rtk proxy cmake --build build-server --target wivrn-server -j2`
Exit code: `0` (`build.exit`). Full output is in `build.log`; source and built target SHA-256 are in `hashes.txt`.
