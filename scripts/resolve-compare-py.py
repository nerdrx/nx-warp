#!/usr/bin/env python3
"""Resolve the tools/quality/compare.py conflict between merge-main and tourney/ctx-b.

merge-main split the encode/decode timing (enc_s / dec_s) and added
``**(extra or {})`` to the measure() call.  tourney/ctx-b independently fixed a
real measurement bug in the same function: ``--frames N`` on a longer sequence
must truncate the source, otherwise the reported bitrate is N/seq.frames of the
true value.  The two changes are disjoint in intent, so the resolution is their
union: keep merge-main's timing and ``extra``, adopt ctx-b's ``src`` truncation
and use ``src`` at every place the source file is named inside run_codec().

Idempotent: running it on an already-resolved file is a no-op.
"""
import re
import sys

PATH = sys.argv[1] if len(sys.argv) > 1 else "tools/quality/compare.py"

TRUNCATE = '''    # The codec CLIs take a whole file, not a frame count, so `--frames N` on a
    # longer sequence has to truncate the source: encoding all of it and then
    # dividing its bytes by N reports N/seq.frames of the real bitrate, which
    # is silent and wrong.  The anchors get their frame count through ffmpeg.
    src = seq.path
    if frames and frames < seq.frames:
        src = os.path.join(work, "src-%dframes.yuv" % frames)
        if not os.path.exists(src):
            with open(seq.path, "rb") as fi, open(src, "wb") as fo:
                fo.write(fi.read(seq.fmt.frame_bytes * frames))
'''

def main() -> int:
    text = open(PATH, encoding="utf-8").read()

    if "<<<<<<<" in text:
        # Take the HEAD (merge-main) side of every conflict; ctx-b's contribution
        # is re-applied below by the rewrites, which is what makes this a union
        # rather than an "ours".
        text = re.sub(r"<<<<<<< [^\n]*\n(.*?)=======\n.*?>>>>>>> [^\n]*\n",
                      lambda m: m.group(1), text, flags=re.S)

    if "src-%dframes.yuv" not in text:
        anchor = ('           "rate_control": "qp", "points": []}\n')
        if anchor not in text:
            print("resolve-compare-py: anchor for the truncation block not found",
                  file=sys.stderr)
            return 1
        text = text.replace(anchor, anchor + TRUNCATE, 1)

    # Inside run_codec() the source is `src`, never `seq.path`.
    start = text.index("def run_codec(")
    end = text.index("# --- analysis", start)
    body = text[start:end].replace("seq.path", "src")
    # `src = seq.path` is the one line that must keep naming seq.path.
    body = body.replace("src = src", "src = seq.path")
    body = body.replace('with open(src, "rb") as fi', 'with open(seq.path, "rb") as fi')
    text = text[:start] + body + text[end:]

    open(PATH, "w", encoding="utf-8").write(text)
    print("resolve-compare-py: resolved %s" % PATH)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
