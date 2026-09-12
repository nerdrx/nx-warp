"""Real Blender/Eevee fixed-camera reversal fixture."""
from pathlib import Path
import sys, math, json, csv, time
import bpy
from mathutils import Vector
ROOT=Path(__file__).resolve().parent; sys.path.insert(0,str(ROOT.parent.parent/'motion-scene'))
import scene

def k(o,f,x,rot=None):
    o.location=x; o.keyframe_insert('location',frame=f)
    if rot is not None: o.rotation_euler=rot; o.keyframe_insert('rotation_euler',frame=f)

scene.OUT=str(ROOT/'frames'); scene.N=60; scene.build(); sc=bpy.context.scene
sc.frame_end=60; sc.render.filepath=str(ROOT/'frames'/'frame_####.png'); sc.render.engine='BLENDER_EEVEE'; sc.render.resolution_x=512; sc.render.resolution_y=512; sc.render.resolution_percentage=100; sc.render.fps=60; sc.render.image_settings.file_format='PNG'; sc.render.image_settings.color_mode='RGBA'; sc.render.image_settings.color_depth='8'; sc.render.use_file_extension=True; sc.render.film_transparent=False
cam=sc.camera; cam.animation_data_clear(); cam.location=(0,-7,2.8); direction=Vector((0,2.8,1.8))-cam.location; cam.rotation_euler=direction.to_track_quat('-Z','Y').to_euler(); cam.data.lens=44
block=bpy.data.objects['moving block']; bar=bpy.data.objects['rotating bar']; block.animation_data_clear(); bar.animation_data_clear(); rows=[]
for f in range(1,61):
    t=(f-1)/59
    if t<.42: u=t/.42; x=-4+7*u; phase='forward'
    elif t<.52: x=3.; phase='stop'
    else: u=(t-.52)/.48; x=3-7*u; phase='reverse'
    y=1.8+.45*math.sin(t*math.tau*1.25); bx=1.5-math.sin(t*math.tau*1.5); by=1.3+1.4*t
    k(block,f,(x,y,1.),(0,0,t*math.tau*1.2)); k(bar,f,(bx,by,1.15),(0,t*math.tau*2,t*math.tau*3)); rows.append(dict(frame=f,time_s=(f-1)/60,block_x=x,block_y=y,phase=phase,bar_x=bx,bar_y=by))
sc.frame_start=1; sc.frame_end=60; bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/'reversal-fixture.blend')); bpy.ops.render.render(animation=True)
(ROOT/'trajectory.json').write_text(json.dumps({'fixture':'reversal-blender','width':512,'height':512,'fps':60,'frames':60,'camera':'fixed','motion_blur':False,'trajectory':rows},indent=2)+'\n')
with (ROOT/'trajectory.csv').open('w',newline='') as h: w=csv.DictWriter(h,fieldnames=rows[0]); w.writeheader(); w.writerows(rows)
(ROOT/'provenance.json').write_text(json.dumps({'created_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'source_scene':'/run/media/nerdrx/Lex/claude/nx-scratch/motion-scene/scene.py','renderer':'Blender 5.2 Eevee Next','settings':'512x512, 60Hz, 60 frames, blur off','objects':['moving block','rotating bar'],'no_ids_masks_predictor':True},indent=2)+'\n')
