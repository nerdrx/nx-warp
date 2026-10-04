#!/usr/bin/env python3
"""Model independent full-width ASTC bands under packet loss; write aggregate CSVs only."""
import argparse, csv, ctypes, ctypes.util, hashlib, math, random
from pathlib import Path

MTU, HEADER, REGION_HEADER, K, DEPTH = 1400, 24, 8, 8, 4
TRIALS, FRAMES, BAND_HEIGHTS = 300, 36, (256, 512, 1024)
lib = ctypes.CDLL(ctypes.util.find_library('zstd'))
lib.ZSTD_compressBound.argtypes=[ctypes.c_size_t]; lib.ZSTD_compressBound.restype=ctypes.c_size_t
lib.ZSTD_compress.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int]; lib.ZSTD_compress.restype=ctypes.c_size_t
lib.ZSTD_isError.argtypes=[ctypes.c_size_t]; lib.ZSTD_isError.restype=ctypes.c_uint

def zstd(b):
    src=ctypes.create_string_buffer(b); cap=lib.ZSTD_compressBound(len(b)); dst=ctypes.create_string_buffer(cap)
    n=lib.ZSTD_compress(dst,cap,src,len(b),3)
    if lib.ZSTD_isError(n): raise RuntimeError('Zstd failed')
    return dst.raw[:n]

def read_astc(path):
    b=path.read_bytes()
    if b[:4]!=bytes((0x13,0xAB,0xA1,0x5C)) or b[4:7]!=bytes((8,8,1)): raise ValueError('expected ASTC 8x8 2D')
    dim=lambda o:int.from_bytes(b[o:o+3],'little')
    w,h,d=dim(7),dim(10),dim(13); bw,bh=(w+7)//8,(h+7)//8; raw=b[16:]
    if d!=1 or len(raw)!=bw*bh*16: raise ValueError('malformed ASTC')
    return w,h,bw,bh,raw

def fec(count):
    groups=[]; ds=ws=0
    while ds<count:
        n=min(K*DEPTH,count-ds); p=min(DEPTH,n)
        for j in range(p): groups.append(([(ws+i,ds+i) for i in range(n) if i%DEPTH==j],ws+n+j))
        ds+=n; ws+=n+p
    return groups,ws

def pattern(n,rng,p,burst):
    if not burst: return [rng.random()<p for _ in range(n)]
    start=p/(burst-p*(burst-1)) if p else 0; out=[]; remaining=0
    for _ in range(n):
        if remaining: out.append(True); remaining-=1
        elif rng.random()<start: out.append(True); remaining=burst-1
        else: out.append(False)
    return out

def plans(path):
    w,h,bw,bh,raw=read_astc(path); out=[]
    whole=zstd(raw); n=math.ceil((HEADER+len(whole))/MTU)
    out.append(('whole',1,len(whole),HEADER,n,[None]*n,[w*h]))
    for band_px in BAND_HEIGHTS:
        blocks=band_px//8; owners=[]; areas=[]; packed_total=0
        for rid,y in enumerate(range(0,bh,blocks)):
            rows=min(blocks,bh-y); segment=raw[y*bw*16:(y+rows)*bw*16]
            packed=zstd(segment); size=REGION_HEADER+len(packed); count=math.ceil(size/MTU)
            packed_total+=len(packed); owners.extend([rid]*count)
            areas.append(w*min(band_px,h-y*8))
        assert sum(areas)==w*h
        out.append((f'band_{band_px}px',len(areas),packed_total,len(areas)*REGION_HEADER,len(owners),owners,areas))
    return w,h,out

def main():
    ap=argparse.ArgumentParser(description=__doc__); ap.add_argument('--dark-q6',type=Path,required=True); ap.add_argument('--forest-q6',type=Path,required=True); ap.add_argument('--out-dir',type=Path,default=Path('.')); a=ap.parse_args()
    payload=[]; loss=[]
    for sample,path in (('dark_fixture',a.dark_q6),('forest_fixture',a.forest_q6)):
        w,h,variants=plans(path); base=0
        for name,nregions,comp,heads,nfrags,owners,areas in variants:
            groups,wires=fec(nfrags); total=comp+heads+len(groups)*MTU
            if name=='whole': base=total
            payload.append(dict(sample=sample,width=w,height=h,mode=name,regions=nregions,zstd3_band_bytes=comp,estimated_region_headers_bytes=heads,data_fragments_1400=nfrags,fec_groups=len(groups),estimated_parity_payload_bytes=len(groups)*MTU,total_estimated_payload_bytes=total,penalty_vs_whole_percent=0))
            for label,p,burst in (('iid_2pct',.02,0),('iid_5pct',.05,0),('burst8_2pct',.02,8),('burst16_2pct',.02,16)):
                seed=int.from_bytes(hashlib.sha256(f'{sample}:{name}:{label}'.encode()).digest()[:4],'little'); rng=random.Random(20261004+seed)
                fresh_sum=whole_sum=lost_sum=wire_sum=0
                for _ in range(TRIALS*FRAMES):
                    dropped=pattern(wires,rng,p,burst); recovered=[True]*nfrags
                    for members,parity in groups:
                        erased=[di for wi,di in members if dropped[wi]]
                        if len(erased)==1 and not dropped[parity]: erased=[]
                        for di in erased: recovered[di]=False
                    if name=='whole': fresh=float(all(recovered)); whole_sum+=fresh
                    else:
                        good=[True]*nregions
                        for ok,rid in zip(recovered,owners):
                            if not ok: good[rid]=False
                        fresh=sum(area for ok,area in zip(good,areas) if ok)/(w*h)
                    fresh_sum+=fresh; lost_sum+=sum(dropped); wire_sum+=wires
                loss.append(dict(sample=sample,mode=name,loss_model=label,requested_loss=p,burst_packets=burst,trials=TRIALS,repeated_static_sends_per_trial=FRAMES,sends_simulated=TRIALS*FRAMES,mean_recovered_area_per_send=fresh_sum/(TRIALS*FRAMES),fully_fresh_whole_send_fraction=whole_sum/(TRIALS*FRAMES) if name=='whole' else '',observed_packet_loss_fraction=lost_sum/wire_sum,mean_wire_packets_per_send=wire_sum/(TRIALS*FRAMES)))
        for row in payload:
            if row['sample']==sample: row['penalty_vs_whole_percent']=(row['total_estimated_payload_bytes']/base-1)*100
    a.out_dir.mkdir(parents=True,exist_ok=True)
    for filename,rows in (('native_band_payload.csv',payload),('native_band_loss.csv',loss)):
        with (a.out_dir/filename).open('w',newline='') as f:
            writer=csv.DictWriter(f,fieldnames=rows[0].keys()); writer.writeheader(); writer.writerows(rows)
    print(f'wrote {len(payload)} payload and {len(loss)} loss rows; {TRIALS} trials x {FRAMES} unchanged sends')
if __name__=='__main__': main()
