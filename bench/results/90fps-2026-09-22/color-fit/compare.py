import numpy as np
from PIL import Image,ImageDraw,ImageFont
from pathlib import Path
import json,sys
p=Path(sys.argv[1])
W=H=2176
def decode(path):
 u=np.frombuffer(path.read_bytes(),dtype='<u4');n=int(u[2]);desc=u[4:4+n];b=u[4+n:].reshape(-1,5);ends=b[:,0]
 def rgb(v):
  r=(v>>11)&31;g=(v>>5)&63;bl=v&31
  return np.stack([(r<<3)|(r>>2),(g<<2)|(g>>4),(bl<<3)|(bl>>2)],axis=-1)
 a=rgb(ends&65535);z=rgb(ends>>16);pal=np.stack([((3-q)*a+q*z+1)//3 for q in range(4)],axis=1).astype('uint8')
 selectors=((b[:,1:,None]>>np.arange(0,32,2,dtype=np.uint32))&3).reshape(-1,64)
 decoded=pal[np.arange(len(b))[:,None],selectors].reshape(-1,8,8,3)
 out=np.empty((H,W*2,3),dtype='uint8')
 for i,d in enumerate(desc):
  mode=int(d>>30);y=i//136*32;x=i%136*32
  if mode==3:out[y:y+32,x:x+32]=[(d>>16)&255,(d>>8)&255,d&255];continue
  k=4>>mode;off=int(d&0x3fffffff)//5;t=decoded[off:off+k*k].reshape(k,k,8,8,3).transpose(0,2,1,3,4).reshape(k*8,k*8,3);out[y:y+32,x:x+32]=t.repeat(1<<mode,0).repeat(1<<mode,1)
 return out
def source(path):
 data=np.frombuffer(path.read_bytes(),dtype='uint8');eyes=[];lb=W*H*3//2
 for e in range(2):
  y=data[e*lb:e*lb+W*H].reshape(H,W).astype('float32')/255
  uv=data[e*lb+W*H:(e+1)*lb].reshape(H//2,W//2,2).repeat(2,0).repeat(2,1).astype('float32')/255-.5
  eyes.append(np.clip(np.stack([y+1.5748*uv[:,:,1],y-.1873*uv[:,:,0]-.4681*uv[:,:,1],y+1.8556*uv[:,:,0]],axis=-1)*255,0,255))
 return np.concatenate(eyes,axis=1)
results=[];panels=[]
for name in ['colors','edges','photo','scene']:
 ref=source(p/(name+'.nv12'))
 for rate in [160,200,500]:
  oldpath=(p/'before')/(f'{name}-{rate}.nxdf')
  old=decode(oldpath);new=decode(p/'after'/f'{name}-{rate}.nxdf');mse0=float(np.mean((old.astype('float32')-ref)**2));mse1=float(np.mean((new.astype('float32')-ref)**2));results.append(dict(fixture=name,rate=rate,before_mse=mse0,after_mse=mse1,bytes_before=oldpath.stat().st_size,bytes_after=(p/'after'/f'{name}-{rate}.nxdf').stat().st_size))
  if rate==500 and name in ['colors','edges']:
   panels.append([Image.fromarray(np.uint8(a[896:1152,896:1152])).resize((384,384),Image.Resampling.NEAREST) for a in [ref,old,new]])
font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',18);im=Image.new('RGB',(1184,860),'#14101f');d=ImageDraw.Draw(im)
for i,label in enumerate(['NV12 source','Previous palette','Actual-color candidate']):d.text((16+i*392,10),label,font=font,fill='white')
for row,imgs in enumerate(panels):
 for col,img in enumerate(imgs):im.paste(img,(8+392*col,42+row*402))
d.text((12,833),'GPU encoder output · 500 setting · centre crops enlarged · unchanged block bytes',font=font,fill='white');im.save(p/'comparison.png');(p/'quality.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
