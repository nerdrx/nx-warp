"""Compare the two shader mappings in float32, including stereo coordinates."""
from pathlib import Path
import json,numpy as np
F=np.float32
rng=np.random.default_rng(240)
count=changed=masks=cells=0;maximum=0.;total=0.
def check(p):
 global count,changed,masks,cells,maximum,total
 p=np.asarray(p,dtype='float32')
 centre=np.column_stack(((np.floor(p[:,0]/F(2176))+F(.5))*F(2176),np.full(len(p),1088,dtype='float32')))
 tc=(np.floor(p/F(64))+F(.5))*F(64);d=tc-centre;r2=np.sum(d*d,axis=1,dtype='float32');radius=np.sqrt(r2)
 old_mask=radius>F(256);new_mask=r2>F(65536)
 old_cell=np.where(radius*F(.88622692545)<=F(512),F(4),F(8));inv=np.where(r2<=F(333772.1072),F(.25),F(.125))
 stride=np.where(np.abs(p-centre)<F(256),F(1),F(4))
 phase=(p-old_cell[:,None]*F(.5))/old_cell[:,None];fraction=phase-np.floor(phase)
 old=np.floor(phase)*old_cell[:,None]+old_cell[:,None]+(fraction-F(.5))*stride
 phase_new=p*inv[:,None]-F(.5);fraction_new=phase_new-np.floor(phase_new)
 new=p+(F(.5)-fraction_new)*(F(1)/inv[:,None]-stride)
 old=np.where(old_mask[:,None],old,p);new=np.where(new_mask[:,None],new,p)
 e=np.abs(old-new);count+=len(p);changed+=int(np.count_nonzero(e));maximum=max(maximum,float(e.max()));total+=float(e.sum(dtype='float64'))
 masks+=int(np.count_nonzero(old_mask!=new_mask));cells+=int(np.count_nonzero(old_cell!=F(1)/inv))
# Every source pixel centre of both eyes, in bounded chunks.
for row in range(0,2176,64):
 y,x=np.meshgrid(np.arange(row,min(row+64,2176),dtype='float32')+F(.5),np.arange(4352,dtype='float32')+F(.5),indexing='ij');check(np.column_stack((x.ravel(),y.ravel())))
# Both eyes at subpixel coordinates, including mask/cell boundary neighborhoods.
check(rng.uniform([.5,.5],[4351.5,2175.5],size=(200000,2)).astype('float32'))
assert masks==cells==0
assert maximum<=.001
result={'points':count,'all_stereo_pixel_centres':True,'random_subpixel_points':200000,'mask_mismatches':masks,'cell_mismatches':cells,'max_source_pixel_error':maximum,'mean_source_pixel_error':total/(count*2),'changed_coordinates':changed,'scope':'Float32 coordinate equivalence; not a GPU colour readback comparison'}
Path(__file__).with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
