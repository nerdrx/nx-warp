#!/usr/bin/env python3
"""Inspect a locally copied installed APK. Never connect to or change a device."""
import argparse, hashlib, json, re, subprocess, zipfile
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("apk", type=Path)
p.add_argument("known_manifest", type=Path)
p.add_argument("build_tools", type=Path)
p.add_argument("output", type=Path)
a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=True)
known = json.loads(a.known_manifest.read_text())
with zipfile.ZipFile(a.apk) as z:
    native = z.read("lib/arm64-v8a/libwivrn.so")
apk_hash = hashlib.sha256(a.apk.read_bytes()).hexdigest()
native_hash = hashlib.sha256(native).hexdigest()
assert apk_hash == known["artifact_sha256"]
assert native_hash == known["native_sha256"]
assert known["source_commit"].encode() in native
copy = a.output / "libwivrn.so"; copy.write_bytes(native)
notes = subprocess.check_output(["readelf", "-n", str(copy)], text=True)
(a.output / "notes.log").write_text(notes)
logs = {}
for name, args in [("badging", ["aapt", "dump", "badging"]), ("signature", ["apksigner", "verify", "--print-certs"])]:
    r = subprocess.run([str(a.build_tools / args[0]), *args[1:], str(a.apk)], capture_output=True, text=True)
    assert r.returncode == 0, (name, r.returncode)
    logs[name] = r.stdout + r.stderr
    (a.output / (name + ".log")).write_text(logs[name])
assert "name='org.meumeu.wivrn.nx'" in logs["badging"].splitlines()[0]
assert known["certificate_sha256"] in logs["signature"]
properties = ["debug.wivrn.nx.recovery_poll", "debug.wivrn.nx.astc_deadline", "debug.wivrn.nx.astc_queue_timing"]
result = {"apk_sha256": apk_hash, "native_sha256": native_hash, "source_commit": known["source_commit"],
          "native_build_id": next(x.split(":", 1)[1].strip() for x in notes.splitlines() if "Build ID:" in x),
          "apk_and_native_match_known_install": True, "signature_matches_known_certificate": True,
          "manifest": logs["badging"].splitlines()[0],
          "current_option_tokens_present": {x: x.encode() in native for x in properties},
          "scope": "installed artifact identity only; not loaded runtime, profile, display or latency proof"}
assert not any(result["current_option_tokens_present"].values())
(a.output / "review.json").write_text(json.dumps(result, indent=2) + "\n")
print("Installed APK, native hash, embedded source and signing certificate match archived installation.")
