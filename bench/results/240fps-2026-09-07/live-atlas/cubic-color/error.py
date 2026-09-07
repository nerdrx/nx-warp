import numpy as np
x=np.linspace(0,1,100001)
ref=np.where(x<=0.04045,x/12.92,((x+0.055)/1.055)**2.4)
y=x*(x*(.3053040462611011*x+.6821741186702331)+.012521835068665776)
e=y-ref
print({'samples':len(x),'max_abs_linear_error':float(np.max(np.abs(e))),'rms_linear_error':float(np.sqrt(np.mean(e*e)))})
