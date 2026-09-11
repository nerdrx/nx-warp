import numpy as np
from zero_gate import gate
rng=np.random.default_rng(7);prev=rng.random((512,512,3),dtype=np.float32);field=np.zeros((64,64,2),np.float32);field[:,:,0]=2/512
_,keep,_,_=gate(prev,prev,field);assert not keep.any(), 'Static texture must reject motion'
cur=np.roll(prev,2,axis=1);_,keep,_,_=gate(prev,cur,field);assert keep[1:-1,1:-1].all(), 'Known translation must pass'
ramp=np.broadcast_to(np.arange(512,dtype=np.float32)[None,:,None]/511,(512,512,3)).copy()
_,wrong,_,_=gate(ramp,np.roll(ramp,2,axis=1),-field);assert not wrong[1:-1,1:-1].any(), 'Wrong direction on a ramp must fail'
flat=np.ones_like(prev)*.1;_,keep,_,_=gate(flat,flat,field);assert not keep.any(), 'Flat patch must remain still'
print('PASS: static, correct translation, wrong direction, flat texture')
