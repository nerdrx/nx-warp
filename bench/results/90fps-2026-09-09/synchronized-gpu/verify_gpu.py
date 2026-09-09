"""Independent NumPy oracle using searchsorted native-coordinate mapping."""
import csv,json,sys,hashlib
from pathlib import Path
import numpy as np
from native_mapping import axis,nearest
p=Path(sys.argv[1]);prefix=str(p)
# Memory layout is side-by-side eyes; guide is two consecutive per-eye arrays.
h=np.fromfile(prefix+'-history.u32',dtype='<u4').reshape(928,1856).reshape(928,2,928).transpose(1,0,2)
f=np.fromfile(prefix+'-fresh.u32',dtype='<u4').reshape(928,1856).reshape(928,2,928).transpose(1,0,2)
g=np.fromfile(prefix+'-guide.u32',dtype='<u4').reshape(2,464,464);up=g.repeat(2,1).repeat(2,2)
centre=np.zeros((928,928),bool);centre[208:720,208:720]=True
checks=[]
for row in csv.DictReader(open(prefix+'-cases.csv')):
 k=int(row['case']);dx=int(row['dx']);native=axis-dx;valid=(native>=0)&(native<2176);indices=nearest(native)
 warped=h[:,:,indices]
 mean=warped.reshape(2,464,2,464,2).sum((2,4))//4
 block_valid=valid.reshape(464,2).all(1).repeat(2)
 take=valid[None,None,:]&block_valid[None,None,:]&(np.abs(up.astype('int32')-mean.repeat(2,1).repeat(2,2).astype('int32'))<=int(row['threshold']))
 if int(row['reset']) or int(row['age'])!=1:take[:]=False
 expected=np.where(take,warped,up);expected[:,centre]=f[:,centre]
 if int(row['full']):expected=f.copy()
 path=prefix+f'-case{k}.u32';raw=np.fromfile(path,dtype='<u4').reshape(928,1856);got=raw.reshape(928,2,928).transpose(1,0,2)
 mismatch=int((expected!=got).sum());assert not mismatch,(k,dx,mismatch)
 assert np.array_equal(got[:,centre],f[:,centre])
 checks.append({'case':k,'dx':dx,'age':int(row['age']),'reset':int(row['reset']),'full':int(row['full']),'exact':True,'sha256':hashlib.sha256(Path(path).read_bytes()).hexdigest()})
r=list(csv.DictReader(open(prefix+'-timing.csv')));stats={}
for label,rows in [('full',[x for x in r if x['full']=='1']),('guide_history',[x for x in r if x['full']=='0']),('combined',r)]:
 a=np.array([float(x['gpu_ms']) for x in rows]);stats[label]={'n':len(a),'mean_ms':float(a.mean()),'p50_ms':float(np.percentile(a,50)),'p95_ms':float(np.percentile(a,95)),'p99_ms':float(np.percentile(a,99))}
result={'checks':checks,'timing':stats,'limits':'GPU reconstruction dispatch only; uint32 grayscale; resident static inputs. Excludes input creation/upload, decode, history-cache updates and presentation. Not full pipeline latency or motion quality.'}
Path(prefix+'-verified.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(stats,indent=2));print('14 independent GPU/reference checks passed')
