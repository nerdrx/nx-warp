from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
D=Path('/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-haar-20261003/motion-visual'); W,H=4352,2176; sizes=[W*H,W*H//4,W*H//4]
src=np.fromfile(D/'private-dark-native-stereo-420.yuv',np.uint8); decoded=[np.fromfile(D/f'readback-420.frame000.{s}.raw',np.uint8) for s in ('y','cb','cr')]
source=[src[:sizes[0]].reshape(H,W),src[sizes[0]:sizes[0]+sizes[1]].reshape(H//2,W//2),src[sizes[0]+sizes[1]:].reshape(H//2,W//2)]
dec=[a.reshape(H,W) if i==0 else a.reshape(H//2,W//2) for i,a in enumerate(decoded)]
mae=[float(np.abs(dec[i].astype(np.int16)-source[i].astype(np.int16)).mean()) for i in range(3)]
def rgb(p):
 y,cb,cr=p; cb=Image.fromarray(cb).resize((W,H),Image.Resampling.BILINEAR); cr=Image.fromarray(cr).resize((W,H),Image.Resampling.BILINEAR)
 y=y.astype(np.float32); cb=np.asarray(cb,dtype=np.float32)-128; cr=np.asarray(cr,dtype=np.float32)-128
 return np.clip(np.stack((y+1.5748*cr,y-.187324*cb-.468124*cr,y+1.8556*cb),2),0,255).astype(np.uint8)
a=Image.fromarray(rgb(source)).resize((640,320),Image.Resampling.LANCZOS); b=Image.fromarray(rgb(dec)).resize((640,320),Image.Resampling.LANCZOS)
out=Image.new('RGB',(1280,350),(15,18,24)); out.paste(a,(0,30));out.paste(b,(640,30));dr=ImageDraw.Draw(out);dr.text((12,8),'Source 4:2:0',fill='white');dr.text((652,8),'Paired Haar decode',fill='white');out.save(D/'preview-420.png',optimize=True)
text=f'frame=4352x2176 stereo 4:2:0\ninput_bytes={sum(sizes)} output_plane_bytes={sum(sizes)}\nsource_to_420_chroma=floor((sum_2x2+2)/4)\nplane_MAE_Y_Cb_Cr={mae[0]:.6f},{mae[1]:.6f},{mae[2]:.6f}\npreview=1280x350; each eye image downscaled from 2176x2176 to 640x320 including crop-free full view; chroma upsampled bilinear for display\n'
(D/'summary-420.txt').write_text(text);print(text)
