"""Full-frame stereo motion cadence fixture."""
import json, os, re, subprocess, sys
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parent
ANALYZE = "--analyze-only" in sys.argv
argv = [x for x in sys.argv[1:] if x != "--analyze-only"]
BIN = Path(argv[0]).resolve() if argv else None
if BIN is None: raise SystemExit("usage: full_motion.py BIN_DIR")
ENC, DEC = BIN / "nxvc-vkenc-api", BIN / "nxv-dec"
W, H, N, EYES = 1024, 512, 16, 2
def run(args, log, env=None):
    with (ROOT / log).open("w") as f:
        subprocess.run([str(x) for x in args], cwd=ROOT, env=env, stdout=f,
                       stderr=subprocess.STDOUT, check=True)
yy, xx = np.indices((H, W), dtype=np.int32)
y0 = ((xx * 3 + yy * 5 + (xx // 17) * 11) & 255).astype(np.uint8)
u0 = ((xx[::2, ::2] * 5 + yy[::2, ::2] * 3 + 67) & 255).astype(np.uint8)
v0 = ((xx[::2, ::2] * 2 + yy[::2, ::2] * 7 + 149) & 255).astype(np.uint8)
frames = []
with (ROOT / "full_motion.yuv").open("wb") as f:
    for t in range(N):
        y = np.roll(y0, (t * 5, t * 9), (0, 1)); u = np.roll(u0, (t * 2, t * 3), (0, 1)); v = np.roll(v0, (t * 3, t * 4), (0, 1))
        u[96+t*2:192+t*2, 160+t*4:288+t*4] ^= 0x55; v[144:240, 520-t*3:648-t*3] ^= 0x33
        frames.append((y, u, v)); f.write(y.tobytes() + u.tobytes() + v.tobytes())
def encode(tag, cadence):
    env = os.environ.copy(); env.pop("NXVC_PLANAR_CADENCE", None); env.pop("NXVC_PLANAR_CADENCE_TRACE", None)
    if cadence: env.update(NXVC_PLANAR_CADENCE="1", NXVC_PLANAR_CADENCE_TRACE="1")
    run([ENC, "--in", ROOT/"full_motion.yuv", "--w", W, "--h", H, "--eyes", EYES, "--out", ROOT/(tag+".nxv"), "--frames", N, "--qp", 40, "--inter", "--planar-gpu-centre", "--centre-quarter", "--centre-graduated"], tag+".log", env)
    run([DEC, "--in", ROOT/(tag+".nxv"), "--out", ROOT/(tag+"-decoded.yuv"), "--pix", "yuv420p", "--quiet"], tag+"-decode.log")
if not ANALYZE: encode("full-motion-off", False); encode("full-motion-on", True)
sizes = [(H,W),(H//2,W//2),(H//2,W//2)]
def load(p, n=N): return np.fromfile(p, dtype=np.uint8).reshape(n, -1)
off, on = load(ROOT/"full-motion-off-decoded.yuv"), load(ROOT/"full-motion-on-decoded.yuv")
centre = True; errors=[]; cursor=0
for pi,(ph,pw) in enumerate(sizes):
    npx=ph*pw; a=off[:,cursor:cursor+npx].reshape(N,ph,pw); b=on[:,cursor:cursor+npx].reshape(N,ph,pw)
    y0i,y1=(3*ph//8,5*ph//8)
    eye_w = pw // EYES
    native_mask = np.zeros((ph, pw), bool)
    for eye in range(EYES):
        x0 = eye * eye_w + 3 * pw // 16; x1 = x0 + pw // 8
        native_mask[y0i:y1, x0:x1] = True
    centre &= bool(np.array_equal(a[:,native_mask], b[:,native_mask]))
    mask = ~native_mask
    src=np.asarray([q[pi] for q in frames], dtype=np.uint8)
    errors.append(float(np.abs(b.astype(np.int16)-src.astype(np.int16))[:,mask].mean())); cursor+=npx
baseline_errors=[]; cadence_off_errors=[]; cursor=0
for pi,(ph,pw) in enumerate(sizes):
    npx=ph*pw; a=off[:,cursor:cursor+npx].reshape(N,ph,pw); b=on[:,cursor:cursor+npx].reshape(N,ph,pw)
    src=np.asarray([q[pi] for q in frames], dtype=np.uint8); mask=np.ones((ph,pw),bool)
    y0i,y1=(3*ph//8,5*ph//8); eye_w=pw//EYES
    for eye in range(EYES): mask[y0i:y1,eye*eye_w+3*pw//16:eye*eye_w+5*pw//16]=False
    baseline_errors.append(float(np.abs(a.astype(np.int16)-src.astype(np.int16))[:,mask].mean()))
    cadence_off_errors.append(float(np.abs(a.astype(np.int16)-b.astype(np.int16))[:,mask].mean())); cursor+=npx
if not ANALYZE:
    run([DEC,"--in",ROOT/"full-motion-on.nxv","--out",ROOT/"full-motion-every2.yuv","--pix","yuv420p","--decode-every",2,"--quiet"],"full-motion-every2.log")
every2=bool(np.array_equal(load(ROOT/"full-motion-every2.yuv",N//2),on[::2]))
rows=[list(map(int,x)) for x in re.findall(r"cadence: frame (\d+) fit (\d+) reuse (\d+) hot (\d+) max_age (\d+)",(ROOT/"full-motion-on.log").read_text())]
baseline_fits=[]
for frame in range(N):
    count=0
    for eye in range(EYES):
        for row in range(8):
            for col in range(8):
                if 3 <= row < 5 and 3 <= col < 5: continue
                dist=max(3-col if col<3 else col-4 if col>=5 else 0,
                         3-row if row<3 else row-4 if row>=5 else 0)
                period=2 if dist<=2 else 4
                count += frame==0 or (col+3*row+frame)%period==0
    baseline_fits.append(count)
assert len(rows)==N
result={"frames":N,"width":W,"height":H,"eyes":EYES,"native_centre_yuv_unchanged":centre,"decode_every2_exact":every2,"cadence_trace":rows,"fit_total":sum(x[1] for x in rows),"reuse_total":sum(x[2] for x in rows),"hot_detected":any(x[3]>0 for x in rows),"promotion_exceeded_baseline":any(r[1] > baseline_fits[r[0]] for r in rows),"max_age":max((x[4] for x in rows),default=0),"periphery_mae_cadence_vs_source":errors,"periphery_mae_baseline_vs_source":baseline_errors,"periphery_mae_cadence_vs_baseline":cadence_off_errors}
(ROOT/"full-motion.json").write_text(json.dumps(result,indent=2)+"\n")
print(json.dumps({k:v for k,v in result.items() if k!="cadence_trace"},indent=2))
if not centre or not every2 or result["max_age"] > 3 or not result["promotion_exceeded_baseline"]: raise AssertionError(result)
