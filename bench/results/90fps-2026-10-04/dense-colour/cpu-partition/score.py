import math
import numpy as np
from PIL import Image
root='../results/'
src=np.asarray(Image.open('../fixtures/crowd-centre-512.png').convert('RGB'),dtype=np.int16)
def read(name): return np.fromfile(root+name,dtype=np.uint8).reshape(512,512,4)[:,:,:3].astype(np.int16)
cand,base=read('candidate-packed.rgba'),read('baseline.rgba')
def tile_error(a): return np.square(src-a).sum(2).reshape(64,8,64,8).transpose(0,2,1,3).reshape(4096,-1).sum(1)
ec,eb=tile_error(cand),tile_error(base)
choose=ec < eb*.95
cb=np.fromfile(root+'candidate.blocks',dtype=np.uint8).reshape(-1,16)
bb=np.fromfile(root+'baseline.blocks',dtype=np.uint8).reshape(-1,16)
out=np.where(choose[:,None],cb,bb).astype(np.uint8);out.tofile(root+'selected.blocks')
select=choose.reshape(64,64).repeat(8,0).repeat(8,1)[:,:,None]
mixed=np.where(select,cand,base)
for name,img in [('candidate',cand),('baseline',base),('fallback',mixed)]:
 mse=np.mean(np.square(src-img,dtype=np.float64));print(f'{name}: PSNR={10*math.log10(255**2/mse):.4f} dB MAE={np.abs(src-img).mean():.4f}')
print(f'candidate tiles={int(choose.sum())}/4096; candidate raw={len(cb)*16} baseline raw={len(bb)*16} selected raw={len(out)*16}')
