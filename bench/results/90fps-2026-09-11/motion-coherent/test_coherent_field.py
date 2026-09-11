import numpy as np
from coherent_field import clean
c=np.ones((512,512,3),np.float32)*.4;f=np.ones((64,64,2),np.float32)/512
out,_=clean(c,f);assert np.array_equal(f,out)
f[32,32]=[3/512,3/512];out,_=clean(c,f);assert np.all(out[32,32]==1/512);assert np.all(f[32,32]==3/512), 'Input must not change'
f=np.ones((64,64,2),np.float32)/512;f[:,32:]=30/512;out,_=clean(c,f);assert np.array_equal(f,out), 'Incompatible motion boundary must survive'
print('PASS constant translation, isolated outlier, immutable input, motion boundary')
