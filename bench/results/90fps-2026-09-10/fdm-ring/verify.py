import json, numpy as np
n=135; t=16.; extent=2160.; y,x=np.mgrid[:n,:n]; dx=np.abs((x+.5)*t-extent/2); dy=np.abs((y+.5)*t-extent/2); e=np.maximum(dx,dy)
m1=np.where(e<=512,255,np.where(e<=768,128,64)); m3=np.where(e<=512,255,64)
out={'map':'135x135','texel':'16x16','extent':'2160x2160','mode1_counts':{str(k):int((m1==k).sum()) for k in (64,128,255)},'mode3_counts':{str(k):int((m3==k).sum()) for k in (64,128,255)},'protected_cells':int((e<=512).sum()),'protected_unchanged':bool(np.array_equal(m1[e<=512],m3[e<=512])),'middle_cells_reduced':int(((e>512)&(e<=768)).sum())}
open('/run/media/nerdrx/Lex/claude/nx-scratch/fdm-ring/verification.json','w').write(json.dumps(out,indent=2)+'\n'); print(json.dumps(out,indent=2))
