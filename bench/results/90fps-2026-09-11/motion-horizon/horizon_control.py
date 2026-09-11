from pathlib import Path
import sys,bpy
r=Path(__file__).resolve().parent;sys.path.insert(0,str(r.parent/'motion-scene'))
import scene
scene.build();bpy.context.scene.frame_set(3);bpy.context.scene.render.filepath=str(r/'horizon-control.png');bpy.ops.render.render(write_still=True)
