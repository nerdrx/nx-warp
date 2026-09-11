from pathlib import Path
import sys,bpy
r=Path(__file__).resolve().parent
sys.path.insert(0,str(r.parent/'motion-scene'))
import scene
scene.build()
out=r/'horizon-truth';out.mkdir(exist_ok=True)
for i in range(2,34,4):
 bpy.context.scene.frame_set(i+1,subframe=2/3)
 bpy.context.scene.render.filepath=str(out/f'{i-2:03d}.png')
 bpy.ops.render.render(write_still=True)
