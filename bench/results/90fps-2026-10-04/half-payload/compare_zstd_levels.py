#!/usr/bin/env python3
"""Compare ASTC block payload Zstandard levels 1/3 against same-scene q6 LZ4."""
import ctypes as C, ctypes.util, hashlib, json, pathlib, time, statistics
ROOT=pathlib.Path("/tmp/nx-astc-quality-test")
OUT=pathlib.Path(__file__).resolve().parent

def lib(name):
    p=ctypes.util.find_library(name)
    if not p: raise RuntimeError(f"missing {name}")
    return C.CDLL(p)

def check_zstd(z, code):
    if z.ZSTD_isError(code): raise RuntimeError(z.ZSTD_getErrorName(code).decode())
    return int(code)

def sha(b): return hashlib.sha256(b).hexdigest()

def main():
    z=lib("zstd")
    z.ZSTD_compressBound.argtypes=[C.c_size_t]; z.ZSTD_compressBound.restype=C.c_size_t
    z.ZSTD_compress.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t,C.c_int]; z.ZSTD_compress.restype=C.c_size_t
    z.ZSTD_decompress.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t]; z.ZSTD_decompress.restype=C.c_size_t
    z.ZSTD_isError.argtypes=[C.c_size_t]; z.ZSTD_isError.restype=C.c_uint
    z.ZSTD_getErrorName.argtypes=[C.c_size_t]; z.ZSTD_getErrorName.restype=C.c_char_p
    l=lib("lz4")
    l.LZ4_decompress_safe.argtypes=[C.c_void_p,C.c_void_p,C.c_int,C.c_int]; l.LZ4_decompress_safe.restype=C.c_int
    result={"method":"compress exact ASTC blocks (exclude 16-byte ASTC header); verify Zstd/LZ4 round trips", "source_root":str(ROOT),"scenes":{}}
    for scene in ("dark","forest"):
        quality=json.loads((ROOT/f"metrics-{scene}/astc-quality.json").read_text())
        cases={int(x["astc_path"].rsplit("q",1)[1].split(".")[0]):x for x in quality["cases"]}
        assert len({cases[q]["source_sha256"] for q in (2,3,4,5,6)}) == 1, (scene,"source mismatch")
        assert all(cases[q]["width"]==2176 and cases[q]["height"]==2176 and cases[q]["block_width"]==8 and cases[q]["block_height"]==8 for q in (2,3,4,5,6))
        entries={}
        for q in (6,5,4,3,2):
            path=ROOT/f"results/{scene}-q{q}.astc"
            full=path.read_bytes(); assert len(full)==1_183_760 and full[:4]==bytes((0x13,0xAB,0xA1,0x5C)), (path,len(full),full[:4])
            assert full[4:7]==bytes((8,8,1)) and int.from_bytes(full[7:10],"little")==2176 and int.from_bytes(full[10:13],"little")==2176 and int.from_bytes(full[13:16],"little")==1, (path,"invalid ASTC header")
            raw=full[16:]
            entry={"astc_file":str(path),"astc_sha256":sha(full),"blocks_sha256":sha(raw),"astc_bytes_with_header":len(full),"blocks_bytes":len(raw),"rgb_psnr_db":cases[q]["rgb_psnr_db"],"source_png":cases[q]["source_path"],"source_sha256":cases[q]["source_sha256"],"lz4":{}}
            lp=ROOT/f"results/{scene}-q{q}.astc.blocks.lz4"; packed=lp.read_bytes()
            dst=C.create_string_buffer(len(raw)); src=C.create_string_buffer(packed)
            n=l.LZ4_decompress_safe(src,dst,len(packed),len(raw))
            assert n==len(raw) and dst.raw==raw, (scene,q,"LZ4 roundtrip",n)
            entry["lz4"]={"path":str(lp),"bytes":len(packed),"sha256":sha(packed),"roundtrip_exact":True}
            if q in (2,3,4,5):
                zstds={}
                for level in (1,3):
                    bound=int(z.ZSTD_compressBound(len(raw))); dstc=C.create_string_buffer(bound); srcc=C.create_string_buffer(raw)
                    samples=[]; packedz=b""
                    for _ in range(3):
                        t0=time.perf_counter_ns(); n=check_zstd(z,z.ZSTD_compress(dstc,bound,srcc,len(raw),level)); samples.append((time.perf_counter_ns()-t0)/1e6); packedz=dstc.raw[:n]
                    ms=statistics.median(samples)
                    out=C.create_string_buffer(len(raw)); srcz=C.create_string_buffer(packedz)
                    dec=check_zstd(z,z.ZSTD_decompress(out,len(raw),srcz,len(packedz)))
                    assert dec==len(raw) and out.raw==raw, (scene,q,level,"Zstd roundtrip")
                    q6bytes=entries["6"]["lz4"]["bytes"]
                    zstds[str(level)]={"bytes":n,"sha256":sha(packedz),"compress_ms_median_3_host_calls":round(ms,3),"compress_samples_ms":[round(x,3) for x in samples],"roundtrip_exact":True,"pct_saved_vs_raw_astc_blocks":round(100*(1-n/len(raw)),3),"pct_saved_vs_same_scene_q6_lz4":round(100*(1-n/q6bytes),3),"at_least_50pct_saved_vs_q6_lz4":n<=q6bytes/2}
                entry["zstd"]=zstds
            entries[str(q)]=entry
        result["scenes"][scene]={"quality_cases_file":str(ROOT/f"metrics-{scene}/astc-quality.json"),"cases":entries}
    (OUT/"zstd-q2-q3-vs-q6-lz4.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps({s:{q:{"lz4":c["lz4"]["bytes"],"psnr":c["rgb_psnr_db"],"zstd":c.get("zstd")} for q,c in d["cases"].items()} for s,d in result["scenes"].items()},indent=2))
if __name__=="__main__": main()
