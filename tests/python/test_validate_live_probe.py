import hashlib, json, subprocess, sys, tempfile
from pathlib import Path

SCRIPT = Path(__file__).parents[2] / "tools" / "validate-live-probe.py"

def run(tmp, **kw):
    cmd = [sys.executable, str(SCRIPT), "--binary", str(kw.get("binary", tmp/"bin")),
           "--sha256", kw.get("sha", "0"*64), "--log", str(kw.get("log", tmp/"log")),
           "--timings", str(kw.get("timings", tmp/"timings.csv"))]
    if "banner" in kw: cmd += ["--banner", kw["banner"]]
    if "dims" in kw: cmd += ["--dims", kw["dims"]]
    return subprocess.run(cmd, text=True, capture_output=True)

def fixture(tmp, log="NX READY 2160x2160\n", csv="receive_begin,0,1,0\n"):
    b=tmp/"bin"; b.write_bytes(b"probe")
    (tmp/"log").write_text(log); (tmp/"timings.csv").write_text(csv)
    return b, hashlib.sha256(b.read_bytes()).hexdigest()

def test_valid():
    with tempfile.TemporaryDirectory() as d:
        t=Path(d); b,h=fixture(t); r=run(t,binary=b,sha=h,banner="NX READY",dims="2160x2160")
        assert r.returncode == 0, r.stderr

def test_fail_closed_cases():
    cases=[{"binary":Path("missing")}, {"banner":"REQUIRED"}, {"dims":"999x999"}, {"timings":Path("missing.csv")}]
    for case in cases:
        with tempfile.TemporaryDirectory() as d:
            t=Path(d); b,h=fixture(t); case.setdefault("binary",b); case.setdefault("sha",h)
            r=run(t,**case); assert r.returncode != 0, case

if __name__ == "__main__":
    test_valid()
    test_fail_closed_cases()
    print("validate-live-probe synthetic tests: PASS")
