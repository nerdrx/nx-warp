"""Export camera calibration; render a static-scene rotation-only control."""
from pathlib import Path
import sys,json,bpy
from mathutils import Vector
r=Path(__file__).resolve().parent
sys.path.insert(0,str(r.parent/'motion-scene'))
import scene
scene.build();sc=bpy.context.scene;cam=sc.camera
out=r/'rotation-control';out.mkdir(exist_ok=True)
def pose():
 return {'rotation':[list(v) for v in cam.matrix_world.to_3x3()], 'projection':[list(v) for v in cam.calc_matrix_camera(bpy.context.evaluated_depsgraph_get(),x=512,y=512)]}
poses={}
for f in range(1,37):
 sc.frame_set(f);poses[str(f)]=pose()
(out/'moving-poses.json').write_text(json.dumps(poses))
sc.frame_set(15)
for obj in sc.objects:
 obj.animation_data_clear()
loc=cam.location.copy();base=cam.rotation_euler.copy();poses={}
for f in range(8):
 cam.location=loc;cam.rotation_euler=base.copy();cam.rotation_euler.rotate_axis('Y',(f-3.5)*0.012)
 bpy.context.view_layer.update();poses[str(f)]=pose()
 sc.render.filepath=str(out/f'{f:03d}.png');bpy.ops.render.render(write_still=True)
(out/'poses.json').write_text(json.dumps(poses))
