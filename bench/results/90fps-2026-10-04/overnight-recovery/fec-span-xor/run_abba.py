#!/usr/bin/env python3
import csv, os, pathlib, subprocess
root = pathlib.Path('/tmp/nx-fec-span-xor')
out = pathlib.Path(__file__).with_name('matched.csv')
os.sched_setaffinity(0, {3})
with out.open('w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['block','position','treatment','seed','k','stride','missing_position','max_blob_bytes','ns_per_reconstruct','allocs_per_reconstruct','allocated_bytes_per_reconstruct'])
    for block in range(5):
        for pos, treatment in enumerate(['baseline-final','final','final','baseline-final']):
            proc = subprocess.run([str(root / f'bench-{treatment}'), str(19000+block)], check=True, text=True, capture_output=True)
            for line in proc.stdout.splitlines():
                k,stride,missing,max_blob,ns,allocs,allocated = line.split(',')
                w.writerow([block,pos,treatment,19000+block,k,stride,missing,max_blob,ns,allocs,allocated])
print(out)
