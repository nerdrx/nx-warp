from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import sys, subprocess
source=Path(sys.argv[1]);output=Path(sys.argv[2]);output.mkdir(parents=True,exist_ok=True)
frames=output/"rendered";frames.mkdir(exist_ok=True)
font=ImageFont.truetype("/usr/share/fonts/TTF/DejaVuSans.ttf",18)
small=ImageFont.truetype("/usr/share/fonts/TTF/DejaVuSans.ttf",15)
for n in range(90):
 im=Image.new("RGB",(1330,676),"#15131c");d=ImageDraw.Draw(im)
 d.text((20,8),"Actual partial-recovery helper • moving edges and objects",font=font,fill="white")
 for col,label in enumerate(["Current decoded target","Whole-frame hold","Unguarded recovery","Guarded recovery"]):
  d.text((20+col*328,38),label,font=font,fill="#d1b7ff")
 for row,(gap,label) in enumerate([("gap1","Loss every other frame • history 1 frame old on loss"),("gap3","Three lost frames between intact frames • history up to 3 frames old")]):
  y=72+row*285
  d.text((20,y),label,font=small,fill="white")
  for col,kind in enumerate(["target","hold","partial","guarded"]):
   pic=Image.open(source/gap/f"frame-{n}-{kind}.ppm").resize((320,256),Image.Resampling.NEAREST)
   im.paste(pic,(12+328*col,y+24))
 d.text((20,654),f"CPU unit-test scene, 160×128 • 90 Hz source, played 10× slower • frame {n:02d} • camera {'panning' if n<45 else 'stationary'}",font=small,fill="#bbbbbb")
 im.save(frames/f"{n:03d}.png")
 if n==19:im.save(output/"comparison.png")
subprocess.run(["ffmpeg","-y","-loglevel","error","-framerate","9","-i",str(frames/"%03d.png"),"-c:v","libx264","-pix_fmt","yuv420p","-crf","18","-movflags","+faststart",str(output/"recovery-motion.mp4")],check=True)
