#!/usr/bin/env python3
"""Compare six Y/Cb/Cr readback frames byte-for-byte."""
import argparse, hashlib
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('reference_dir',type=Path)
p.add_argument('candidate_dir',type=Path)
p.add_argument('reference_prefix')
p.add_argument('candidate_prefix')
a=p.parse_args()
failed=False
for i in range(6):
 for plane in ('y','cb','cr'):
  r=a.reference_dir/f'{a.reference_prefix}.frame{i:03}.{plane}.raw'
  c=a.candidate_dir/f'{a.candidate_prefix}.frame{i:03}.{plane}.raw'
  rb,cb=r.read_bytes(),c.read_bytes()
  ok=rb==cb
  print(f'frame={i} plane={plane} reference_bytes={len(rb)} candidate_bytes={len(cb)} byte_exact={str(ok).lower()} reference_sha256={hashlib.sha256(rb).hexdigest()} candidate_sha256={hashlib.sha256(cb).hexdigest()}')
  failed |= not ok
print(f'{"FAIL" if failed else "PASS"}: {"not " if failed else ""}all 18 planes are byte exact')
raise SystemExit(failed)
