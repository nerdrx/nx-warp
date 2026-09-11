import numpy as np
rng=np.random.default_rng(314)
worst=0.
for _ in range(1000):
 theta=rng.uniform(-np.pi,np.pi);scale=rng.uniform(.9,1.1)
 R=scale*np.array([[np.cos(theta),-np.sin(theta)],[np.sin(theta),np.cos(theta)]])
 shift=rng.uniform(-.2,.2,2);t=np.linspace(0,1,101)
 start=rng.random(2);end=rng.random(2);line=start+(end-start)*t[:,None]
 transformed=line@R.T+shift
 direction=transformed[-1]-transformed[0]
 residual=abs((transformed[:,0]-transformed[0,0])*direction[1]-(transformed[:,1]-transformed[0,1])*direction[0])/max(np.linalg.norm(direction),1e-12)
 worst=max(worst,float(residual.max()))
 assert residual.max()<1e-12
print('1000 similarity transforms preserve straight segments; worst normalized deviation:',worst)
print('This algebraic check excludes segmentation, rasterization, visibility and hole filling.')
