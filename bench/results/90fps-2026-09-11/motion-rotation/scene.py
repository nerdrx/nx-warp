import bpy, math, os
from mathutils import Vector

OUT = os.path.join(os.path.dirname(__file__), "frames")
W = H = 512
FPS = 60
N = 36

def mat(name, color, metallic=0.0, rough=0.35):
    m = bpy.data.materials.new(name); m.diffuse_color = (*color, 1)
    m.use_nodes = True
    bs = m.node_tree.nodes.get('Principled BSDF')
    bs.inputs['Base Color'].default_value = (*color, 1)
    bs.inputs['Roughness'].default_value = rough
    bs.inputs['Metallic'].default_value = metallic
    return m

def cube(name, loc, scale, material, bevel=0.08):
    bpy.ops.mesh.primitive_cube_add(location=loc); o=bpy.context.object; o.name=name; o.scale=scale
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if bevel:
        mod=o.modifiers.new('soft edges','BEVEL'); mod.width=bevel; mod.segments=2
    o.data.materials.append(material); return o

def key(obj, frame, loc=None, rot=None):
    if loc is not None: obj.location=loc; obj.keyframe_insert('location', frame=frame)
    if rot is not None: obj.rotation_euler=rot; obj.keyframe_insert('rotation_euler', frame=frame)

def build():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    os.makedirs(OUT, exist_ok=True)
    red=mat('red',(0.8,0.035,0.025),rough=0.24); blue=mat('blue',(0.025,0.16,0.9),rough=0.22)
    gold=mat('gold',(0.95,0.42,0.03),metallic=.35,rough=.2); green=mat('green',(0.03,.65,.16),rough=.3)
    dark=mat('floor',(0.055,.065,.085),rough=.3); white=mat('marker',(.8,.85,.9),rough=.25)
    # Floor and a back wall create strong parallax and occlusions.
    floor=cube('floor',(0,0,-0.18),(8,8,.18),dark,0)
    for x in range(-7,8,2):
        for y in range(-5,8,2): cube('tile',(x,y,.01),(.96,.96,.025), white if (x+y)%4==0 else dark, .01)
    cube('back wall',(0,7,3),(8,.15,3),mat('wall',(.035,.045,.07),rough=.5),0)
    # Static depth landmarks.
    cube('left pillar',(-3.6,3,1.5),(.45,.5,1.5),blue)
    cube('right pillar',(3.6,4,1.8),(.55,.55,1.8),red)
    bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12, location=(0,4,2.0), radius=1.0)
    ball=bpy.context.object; ball.name='center ball'; ball.data.materials.append(gold)
    # Independent movers cross behind/in front of the central ball.
    mover=cube('moving block',(-4,2.0,1.0),(0.65,.65,1.0),green)
    spinner=cube('rotating bar',(2.0,1.3,1.15),(.22,1.5,.22),red)
    for f in range(1,N+1):
        t=(f-1)/(N-1)
        key(mover,f,(-4+8*t, 2.0+0.8*math.sin(t*math.tau),1.0),(0,0,t*math.tau*1.5))
        key(spinner,f,(2.0-3.0*t,1.3+1.2*t,1.15),(0,t*math.tau*2,t*math.tau*3))
    # Camera advances and yaws, making background and objects move differently.
    bpy.ops.object.camera_add(location=(0,-7,2.8)); cam=bpy.context.object; bpy.context.scene.camera=cam
    for f in range(1,N+1):
        t=(f-1)/(N-1); cam.location=(1.0*math.sin(t*math.tau*.7), -7+2.2*t, 2.8+0.35*math.sin(t*math.tau))
        target=Vector((0,2.8,1.8)); direction=target-Vector(cam.location); cam.rotation_euler=direction.to_track_quat('-Z','Y').to_euler()
        cam.keyframe_insert('location',frame=f); cam.keyframe_insert('rotation_euler',frame=f)
    cam.data.lens=44
    # Lighting.
    bpy.ops.object.light_add(type='AREA', location=(0,-1,7)); bpy.context.object.data.energy=1300; bpy.context.object.data.shape='DISK'; bpy.context.object.data.size=6
    bpy.context.object.rotation_euler=(0,0,0)
    bpy.ops.object.light_add(type='AREA', location=(-5,-2,3)); bpy.context.object.data.energy=700; bpy.context.object.data.color=(.2,.35,1); bpy.context.object.data.size=4
    bpy.context.object.rotation_euler=(math.radians(55),0,math.radians(-35))
    bpy.ops.object.light_add(type='AREA', location=(5,4,4)); bpy.context.object.data.energy=900; bpy.context.object.data.color=(1,.25,.1); bpy.context.object.data.size=3
    bpy.context.object.rotation_euler=(math.radians(65),0,math.radians(145))
    sc=bpy.context.scene; sc.render.engine='BLENDER_EEVEE'; sc.render.resolution_x=W; sc.render.resolution_y=H; sc.render.resolution_percentage=100
    sc.render.fps=FPS; sc.render.image_settings.file_format='PNG'; sc.render.film_transparent=False
    sc.world=bpy.data.worlds.new('World'); sc.world.color=(.008,.012,.025); sc.render.filepath=os.path.join(OUT,'frame_####.png')
    sc.render.image_settings.color_mode='RGBA'; sc.render.image_settings.color_depth='8'; sc.view_settings.look='AgX - Medium High Contrast'
    sc.frame_start=1; sc.frame_end=N

if __name__ == '__main__':
    build(); bpy.ops.render.render(animation=True)
