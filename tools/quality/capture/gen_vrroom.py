#!/usr/bin/env python3
"""Render a realistic VR-like fixture corpus with Blender, in the repo's
fixture format.

Every verdict in ADR-0029 up to now rests on `gen_synthetic.py`, whose content
docs/LOWPOLY-MODE.md called "unusually kind": band-limited procedural texture
on a panorama, no specular, no thin geometry, no independently moving objects.
This builds the opposite: a room with text panels at reading distance, a
mirror-like specular floor, thin high-contrast edges, two humanoid-ish meshes
that move independently of the head, and a skybox.

ONE FILE, TWO HALVES.  Run it with the system Python and it drives Blender for
each trajectory and then packs the result; run it under Blender (`--background
--factory-startup --python gen_vrroom.py -- --render ...`) and it builds the
scene and renders.  That is so the whole fixture is reproducible from one file
rather than from a recipe in a commit message.

  python3 gen_vrroom.py --out /path/to/vrroom [--blender BIN] [--frames 32]

CONVENTIONS.  The pose sidecar is version 2, `nxv-openxr-1`: quaternion xyzw,
right-handed, camera-to-world, x_right/y_up/z_back, XrFovf signs.  Blender's
world is Z-up, so the camera object takes `q_x90 * q`, where `q_x90` is +90
degrees about X -- the change of basis from the OpenXR world to Blender's.  The
camera's LOCAL frame already agrees (Blender cameras look down local -Z with
local +Y up), so nothing else has to be reinterpreted.  Getting this wrong does
not crash and does not make an illegal stream; it makes a worse picture, which
is why docs/WARP-AUDIT.md 5 exists and why it is spelled out here.
"""
import argparse
import json
import math
import os
import subprocess
import sys

# --------------------------------------------------------------------------
# Trajectories.  Yaw rate is the thing every ADR-0029 verdict is indexed by, so
# the three motion fixtures are named for it and the fourth holds the head
# still and moves the WORLD instead -- the case a per-tile atlas has never been
# tested on, because gen_synthetic.py's objects move with the panorama.
# --------------------------------------------------------------------------
TRAJECTORIES = {
    # name        yaw deg/s  sway   translation m/s   object motion
    "rest":      dict(yaw=0.0,  sway=0.9, trans=0.004, objects=False),
    "mid":       dict(yaw=25.0, sway=0.4, trans=0.05,  objects=False),
    "fast":      dict(yaw=75.0, sway=0.3, trans=0.02,  objects=False, step=True),
    "objmotion": dict(yaw=0.0,  sway=0.9, trans=0.004, objects=True),
    # A SEATED head, which "rest" is not.  Measured with `nxv-enc --stats`,
    # `rest` runs at 2.678 deg/s and drags the atlas's tile corners 0.97
    # samples off in ONE frame and 3.83 over four, so a whole-sample identity
    # is never available in it and the still-case floor it reports is not the
    # floor.  These two are: 0.043 deg/s and a corner displacement that stays
    # at 0.016 samples for the whole clip -- sixty times smaller -- built from
    # a slow postural drift and a physiological tremor and nothing else.
    "still":     dict(yaw=0.0, sway=0.0, trans=0.0, objects=False, seated=True),
    "objmotion-still": dict(yaw=0.0, sway=0.0, trans=0.0, objects=True,
                            seated=True),
}

FPS = 90.0
EYE_W = EYE_H = 1088
IPD = 0.063
FOV_DEG = 100.0


def qmul(a, b):
    """Hamilton product, both (x, y, z, w)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def q_axis(ax, ay, az, ang):
    s = math.sin(ang * 0.5)
    return (ax * s, ay * s, az * s, math.cos(ang * 0.5))


def qnorm(q):
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / n for c in q)


def pose_track(name, nframes):
    """Head poses in the OpenXR convention: y up, -z forward, camera-to-world."""
    t = TRAJECTORIES[name]
    out = []
    if t.get("seated"):
        # Amplitudes chosen so ANGULAR VELOCITY stays under 0.1 deg/s: the
        # drift contributes 2*pi*f*A = 0.021 deg/s and the tremor
        # 2*pi*8*0.001 = 0.05 deg/s.  That is what a seated head in a headset
        # does between deliberate movements, and it is the regime in which a
        # whole-sample identity warp is actually reachable.
        for i in range(nframes):
            s = i / FPS
            drift = math.radians(0.030) * math.sin(2 * math.pi * 0.11 * s)
            tremor = math.radians(0.0010) * math.sin(2 * math.pi * 8.0 * s + 0.7)
            yaw = drift + tremor
            pitch = (math.radians(0.022) * math.sin(2 * math.pi * 0.09 * s + 2.0)
                     + math.radians(0.0008) * math.sin(2 * math.pi * 7.3 * s))
            roll = math.radians(0.008) * math.sin(2 * math.pi * 0.07 * s + 1.3)
            q = qnorm(qmul(qmul(q_axis(0, 1, 0, yaw), q_axis(1, 0, 0, pitch)),
                           q_axis(0, 0, 1, roll)))
            # 0.2 mm/s of postural sway: the translation a seated person cannot
            # suppress either.
            px = 0.0002 * math.sin(2 * math.pi * 0.13 * s)
            py = 0.00015 * math.sin(2 * math.pi * 0.17 * s + 0.9)
            out.append(dict(t=s, q=q, pos=(px, py, 0.0),
                            yaw=yaw, pitch=pitch, roll=roll))
        return out
    for i in range(nframes):
        s = i / FPS
        yaw = math.radians(t["yaw"]) * s
        if t.get("step") and i >= nframes // 2:
            # A step: the head snaps 8 degrees mid-clip, so the fixture holds
            # one frame pair no continuous model predicts.
            yaw += math.radians(8.0)
        yaw += math.radians(t["sway"]) * math.sin(2.0 * math.pi * 0.7 * s)
        pitch = math.radians(t["sway"] * 0.6) * math.sin(2.0 * math.pi * 0.43 * s + 1.1)
        roll = math.radians(t["sway"] * 0.25) * math.sin(2.0 * math.pi * 0.31 * s + 0.4)
        q = qnorm(qmul(qmul(q_axis(0, 1, 0, yaw), q_axis(1, 0, 0, pitch)),
                       q_axis(0, 0, 1, roll)))
        px = t["trans"] * s
        py = 0.02 * t["trans"] / 0.05 * math.sin(2 * math.pi * 0.5 * s) if t["trans"] else 0.0
        out.append(dict(t=s, q=q, pos=(px, py, 0.0), yaw=yaw, pitch=pitch, roll=roll))
    return out


def write_poses(path, name, poses, nframes):
    half = math.radians(FOV_DEG / 2.0)
    frames = []
    prev = None
    for i, p in enumerate(poses):
        av = 0.0
        if prev is not None:
            d = qmul(p["q"], (-prev[0], -prev[1], -prev[2], prev[3]))
            w = max(-1.0, min(1.0, abs(d[3])))
            av = 2.0 * math.acos(w) * 180.0 / math.pi * FPS
        prev = p["q"]
        frames.append(dict(frame=i, time_s=p["t"],
                           position_xyz=list(p["pos"]),
                           orientation_xyzw=list(p["q"]),
                           yaw_deg=math.degrees(p["yaw"]),
                           pitch_deg=math.degrees(p["pitch"]),
                           roll_deg=math.degrees(p["roll"]),
                           angular_velocity_deg_s=av))
    doc = dict(
        version=2,
        convention=dict(id="nxv-openxr-1", quaternion="xyzw", handedness="right",
                        rotation="camera_to_world", axes="x_right_y_up_z_back",
                        image_origin="top_left", pixel_centre=0.5,
                        fov_sign="xrfovf", pose_kind="render",
                        pairing="n_minus_1_to_n", position_units="m"),
        fov_deg=dict(h=FOV_DEG, v=FOV_DEG),
        fov_rad=dict(left=-half, right=half, up=half, down=-half),
        eye=dict(width=EYE_W, height=EYE_H),
        fps=FPS,
        render=dict(generator="blender-vrroom-1", engine="BLENDER_EEVEE",
                    view_transform="Standard", ipd_m=IPD,
                    note="rendered geometry, not a reprojected panorama: "
                         "specular, thin edges, text and independently moving "
                         "objects are all present and none of them obey the "
                         "head's homography"),
        frames=frames)
    with open(path, "w") as f:
        json.dump(doc, f, indent=1)


# ==========================================================================
# The Blender half.
# ==========================================================================
def build_and_render(outdir, track, nframes):
    import bpy    # noqa: E402  (only exists inside Blender)
    from mathutils import Quaternion

    def clear():
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def mat(name, base, rough=0.6, metal=0.0, emit=None):
        m = bpy.data.materials.new(name)
        m.use_nodes = True
        b = m.node_tree.nodes["Principled BSDF"]
        b.inputs["Base Color"].default_value = (*base, 1.0)
        b.inputs["Roughness"].default_value = rough
        b.inputs["Metallic"].default_value = metal
        if emit is not None:
            b.inputs["Emission Color"].default_value = (*emit, 1.0)
            b.inputs["Emission Strength"].default_value = 1.0
        return m

    def checker_mat(name, a, b, scale=18.0, rough=0.5):
        """A real checker texture: high contrast at a spatial frequency the
        codec cannot dismiss as band-limited."""
        m = bpy.data.materials.new(name)
        m.use_nodes = True
        nt = m.node_tree
        bsdf = nt.nodes["Principled BSDF"]
        tex = nt.nodes.new("ShaderNodeTexChecker")
        tex.inputs["Color1"].default_value = (*a, 1.0)
        tex.inputs["Color2"].default_value = (*b, 1.0)
        tex.inputs["Scale"].default_value = scale
        nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
        bsdf.inputs["Roughness"].default_value = rough
        return m

    clear()
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = EYE_W
    scene.render.resolution_y = EYE_H
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.image_settings.color_depth = "8"
    # Plain sRGB out: this fixture stands in for a compositor's output, not for
    # a film grade, so no Filmic/AgX curve between the render and the codec.
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    try:
        scene.eevee.taa_render_samples = 16
    except AttributeError:
        pass

    # ---- skybox: a gradient world, so the far field is smooth but not flat
    world = bpy.data.worlds.new("W")
    scene.world = world
    world.use_nodes = True
    wnt = world.node_tree
    bg = wnt.nodes["Background"]
    grad = wnt.nodes.new("ShaderNodeTexGradient")
    grad.gradient_type = "SPHERICAL"
    ramp = wnt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.02, 0.03, 0.08, 1)
    ramp.color_ramp.elements[1].color = (0.30, 0.42, 0.62, 1)
    texco = wnt.nodes.new("ShaderNodeTexCoord")
    wnt.links.new(texco.outputs["Generated"], grad.inputs["Vector"])
    wnt.links.new(grad.outputs["Color"], ramp.inputs["Fac"])
    wnt.links.new(ramp.outputs["Color"], bg.inputs["Color"])
    bg.inputs["Strength"].default_value = 0.6

    # ---- room: floor is the mirror-like specular surface
    bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, 0))
    floor = bpy.context.object
    floor.data.materials.append(mat("floor", (0.35, 0.36, 0.40), rough=0.06,
                                    metal=0.85))
    for sx, sy, loc, rot in (
            (12, 4, (0, 5, 2), (math.pi / 2, 0, 0)),
            (12, 4, (0, -5, 2), (math.pi / 2, 0, 0)),
            (10, 4, (6, 0, 2), (math.pi / 2, 0, math.pi / 2)),
            (10, 4, (-6, 0, 2), (math.pi / 2, 0, math.pi / 2))):
        bpy.ops.mesh.primitive_plane_add(size=1, location=loc, rotation=rot)
        w = bpy.context.object
        w.scale = (sx, sy, 1)
        w.data.materials.append(checker_mat(f"wall{loc}", (0.72, 0.70, 0.66),
                                            (0.20, 0.22, 0.26), scale=9.0,
                                            rough=0.75))
    bpy.ops.mesh.primitive_plane_add(size=12, location=(0, 0, 4))
    bpy.context.object.data.materials.append(mat("ceil", (0.55, 0.55, 0.58), 0.9))

    # ---- text panels at reading distance: the case the codec is worst at and
    # the case a user actually looks at.
    txtmat = mat("txt", (0.02, 0.02, 0.02), rough=0.9)
    panelmat = mat("panel", (0.93, 0.93, 0.90), rough=0.8, emit=(0.10, 0.10, 0.10))
    lines = ["NX WARP  ATLAS", "res_level  0 1 2", "13.12.11 PICTURE",
             "seam ratio 1.04", "coded samples/f", "budget 500 B"]
    for pi, (px, py, pz, rz) in enumerate((
            (0.0, 1.5, 1.55, 0.0), (-1.15, 1.5, 1.48, 0.42), (1.15, 1.5, 1.48, -0.42))):
        bpy.ops.mesh.primitive_plane_add(size=1, location=(px, py, pz),
                                         rotation=(math.pi / 2, 0, rz))
        p = bpy.context.object
        p.scale = (0.62, 0.40, 1)
        p.data.materials.append(panelmat)
        for li in range(2):
            body = lines[(pi * 2 + li) % len(lines)]
            tc = bpy.data.curves.new(f"t{pi}{li}", "FONT")
            tc.body = body
            tc.size = 0.075
            tc.align_x = "CENTER"
            ob = bpy.data.objects.new(f"T{pi}{li}", tc)
            ob.data.materials.append(txtmat)
            bpy.context.collection.objects.link(ob)
            ob.location = (px, py - 0.012, pz + 0.10 - 0.17 * li)
            ob.rotation_euler = (math.pi / 2, 0, rz)

    # ---- thin high-contrast edges: a grid of narrow bright bars, the geometry
    # a warp smears first and a blocky codec ruins.
    barmat = mat("bar", (0.95, 0.95, 0.92), rough=0.25, emit=(0.5, 0.5, 0.45))
    for k in range(9):
        bpy.ops.mesh.primitive_cube_add(size=1,
                                        location=(-2.2 + 0.55 * k, 2.4, 1.6))
        b = bpy.context.object
        b.scale = (0.008, 0.02, 0.62)
        b.data.materials.append(barmat)
    for k in range(5):
        bpy.ops.mesh.primitive_cube_add(size=1, location=(0.0, 2.42, 0.95 + 0.32 * k))
        b = bpy.context.object
        b.scale = (2.3, 0.02, 0.006)
        b.data.materials.append(barmat)

    # ---- two humanoid-ish meshes, textured, that move on their own
    movers = []
    for mi, (bx, by) in enumerate(((-2.9, 2.4), (3.0, 3.0))):
        parts = []
        bpy.ops.mesh.primitive_uv_sphere_add(radius=0.16, location=(bx, by, 1.62))
        parts.append(bpy.context.object)
        bpy.ops.mesh.primitive_cylinder_add(radius=0.20, depth=0.72,
                                            location=(bx, by, 1.10))
        parts.append(bpy.context.object)
        for sgn in (-1, 1):
            bpy.ops.mesh.primitive_cylinder_add(radius=0.06, depth=0.62,
                                                location=(bx + 0.27 * sgn, by, 1.14))
            parts.append(bpy.context.object)
            bpy.ops.mesh.primitive_cylinder_add(radius=0.08, depth=0.72,
                                                location=(bx + 0.10 * sgn, by, 0.38))
            parts.append(bpy.context.object)
        m = checker_mat(f"skin{mi}", (0.62, 0.42, 0.32), (0.28, 0.18, 0.14),
                        scale=26.0, rough=0.55)
        for p in parts:
            p.data.materials.append(m)
        movers.append(parts)

    # ---- lights
    for loc, e in (((2.6, -1.2, 2.9), 220.0), ((-2.8, 1.2, 2.9), 180.0),
                   ((0.0, 3.2, 2.6), 140.0)):
        bpy.ops.object.light_add(type="POINT", location=loc)
        bpy.context.object.data.energy = e
        bpy.context.object.data.shadow_soft_size = 0.35
    bpy.ops.object.light_add(type="SUN", location=(0, -6, 6))
    bpy.context.object.data.energy = 0.9
    bpy.context.object.rotation_euler = (math.radians(55), 0, math.radians(20))

    # ---- cameras
    cams = []
    for e in range(2):
        cd = bpy.data.cameras.new(f"cam{e}")
        cd.sensor_fit = "HORIZONTAL"
        cd.angle_x = math.radians(FOV_DEG)
        cd.clip_start = 0.05
        cd.clip_end = 200.0
        co = bpy.data.objects.new(f"CAM{e}", cd)
        co.rotation_mode = "QUATERNION"
        bpy.context.collection.objects.link(co)
        cams.append(co)

    poses = pose_track(track, nframes)
    tcfg = TRAJECTORIES[track]
    # OpenXR world -> Blender world: +90 degrees about X.
    QX90 = Quaternion((math.cos(math.pi / 4), math.sin(math.pi / 4), 0.0, 0.0))
    HEAD = (0.0, 0.0, 1.55)   # standing eye height, Blender Z-up

    os.makedirs(outdir, exist_ok=True)
    for i, p in enumerate(poses):
        qx, qy, qz, qw = p["q"]
        qb = QX90 @ Quaternion((qw, qx, qy, qz))     # mathutils is wxyz
        # Head position: the pose is in OpenXR axes, so map (x, y, z) to
        # Blender's (x, -z, y) before adding the standing height.
        ox, oy, oz = p["pos"]
        base = (HEAD[0] + ox, HEAD[1] - oz, HEAD[2] + oy)
        right = qb @ __import__("mathutils").Vector((1.0, 0.0, 0.0))
        if tcfg["objects"]:
            # The head is still; the world is not.  Two meshes walk in
            # opposite directions and one of them bobs.
            s = p["t"]
            for mi, parts in enumerate(movers):
                dx = (0.85 if mi == 0 else -0.7) * s
                dz = 0.045 * math.sin(2 * math.pi * 1.6 * s + mi)
                for pp in parts:
                    pp.location.x = pp.get("_x0", pp.location.x) + 0.0
                    if "_x0" not in pp:
                        pp["_x0"] = pp.location.x
                        pp["_z0"] = pp.location.z
                    pp.location.x = pp["_x0"] + dx
                    pp.location.z = pp["_z0"] + dz
        for e in range(2):
            off = right * ((e - 0.5) * IPD)
            cams[e].location = (base[0] + off.x, base[1] + off.y, base[2] + off.z)
            cams[e].rotation_quaternion = qb
            scene.camera = cams[e]
            scene.render.filepath = os.path.join(outdir, f"e{e}_{i:04d}.png")
            bpy.ops.render.render(write_still=True)


# ==========================================================================
# The driver half: run Blender per trajectory, then pack the fixture.
# ==========================================================================
def rgb_to_yuv420_bt709_full(rgb):
    """Exact BT.709 full-range, done here rather than left to a converter's
    default so the fixture's colorimetry is a property of this file."""
    import numpy as np
    r = rgb[..., 0].astype(np.float64)
    g = rgb[..., 1].astype(np.float64)
    b = rgb[..., 2].astype(np.float64)
    y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    cb = (b - y) / 1.8556 + 128.0
    cr = (r - y) / 1.5748 + 128.0
    Y = np.clip(np.rint(y), 0, 255).astype(np.uint8)
    cb2 = cb.reshape(cb.shape[0] // 2, 2, cb.shape[1] // 2, 2).mean(axis=(1, 3))
    cr2 = cr.reshape(cr.shape[0] // 2, 2, cr.shape[1] // 2, 2).mean(axis=(1, 3))
    U = np.clip(np.rint(cb2), 0, 255).astype(np.uint8)
    V = np.clip(np.rint(cr2), 0, 255).astype(np.uint8)
    return Y, U, V


def pack(outdir, name, nframes, dest):
    import numpy as np
    w, h = EYE_W * 2, EYE_H
    yuv_path = os.path.join(dest, f"{name}.yuv420p.yuv")
    with open(yuv_path, "wb") as out:
        for i in range(nframes):
            eyes = []
            for e in range(2):
                png = os.path.join(outdir, f"e{e}_{i:04d}.png")
                raw = subprocess.run(
                    ["ffmpeg", "-v", "error", "-i", png, "-f", "rawvideo",
                     "-pix_fmt", "rgb24", "-"],
                    check=True, capture_output=True).stdout
                eyes.append(np.frombuffer(raw, np.uint8).reshape(EYE_H, EYE_W, 3))
            frame = np.concatenate(eyes, axis=1)
            Y, U, V = rgb_to_yuv420_bt709_full(frame)
            out.write(Y.tobytes()); out.write(U.tobytes()); out.write(V.tobytes())
    with open(os.path.join(dest, f"{name}.yuv420p.json"), "w") as f:
        json.dump(dict(name=f"{name}.yuv420p", path=f"{name}.yuv420p.yuv",
                       width=w, height=h, pix_fmt="yuv420p", fps=FPS,
                       frames=nframes, pose_log=f"{name}.poses.json",
                       source=f"blender-vrroom-1:{name}", layout="sbs",
                       color=dict(matrix="bt709", range="full",
                                  transfer="srgb")), f, indent=1)
    return yuv_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--render", metavar="TRACK")
    ap.add_argument("--outdir")
    ap.add_argument("--frames", type=int, default=32)
    ap.add_argument("--out", default="/run/media/nerdrx/Lex/claude/nx-scratch/"
                                     "fixtures/vrroom")
    ap.add_argument("--blender", default="/run/media/nerdrx/Lex/claude/"
                                         "quadwild_tools/blender-5.2.0-linux-x64/blender")
    ap.add_argument("--tracks", default=",".join(TRAJECTORIES))
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    a = ap.parse_args(argv)

    if a.render:
        build_and_render(a.outdir, a.render, a.frames)
        return

    os.makedirs(a.out, exist_ok=True)
    for track in a.tracks.split(","):
        tmp = os.path.join(a.out, f"_png_{track}")
        os.makedirs(tmp, exist_ok=True)
        print(f"[vrroom] rendering {track} ({a.frames} frames x 2 eyes)",
              flush=True)
        subprocess.run(["chrt", "-i", "0", "taskset", "-c", "0-7", "nice",
                        "-n", "19", a.blender, "--background",
                        "--factory-startup", "--python", os.path.abspath(__file__),
                        "--", "--render", track, "--outdir", tmp,
                        "--frames", str(a.frames)], check=True,
                       capture_output=True)
        print(f"[vrroom] packing {track}", flush=True)
        pack(tmp, track, a.frames, a.out)
        write_poses(os.path.join(a.out, f"{track}.poses.json"), track,
                    pose_track(track, a.frames), a.frames)
        print(f"[vrroom] {track}: "
              f"{os.path.getsize(os.path.join(a.out, track + '.yuv420p.yuv'))} B",
              flush=True)


if __name__ == "__main__":
    main()
