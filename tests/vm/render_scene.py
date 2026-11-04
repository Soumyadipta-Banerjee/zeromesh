"""Create a self-contained CPU Cycles scene for the Flamenco/zm-render benchmark.

Run with Blender --background --factory-startup --python render_scene.py -- OUTPUT.blend.
"""
import math
from pathlib import Path
import sys

import bpy
from mathutils import Vector

target = Path(sys.argv[sys.argv.index('--') + 1]).resolve()
target.parent.mkdir(parents=True, exist_ok=True)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = 8
scene.cycles.use_denoising = False
scene.cycles.use_adaptive_sampling = False
scene.cycles.seed = 42
scene.cycles.use_animated_seed = False
scene.render.resolution_x = 384
scene.render.resolution_y = 384
scene.render.resolution_percentage = 100
scene.render.threads_mode = 'FIXED'
scene.render.threads = 2
scene.render.image_settings.file_format = 'PNG'
scene.render.image_settings.color_mode = 'RGBA'
scene.render.image_settings.color_depth = '8'
scene.render.image_settings.compression = 15
scene.render.fps = 24
scene.frame_start = 1
scene.frame_end = 24
scene.world.color = (.15, .15, .15)

def material(name, color, roughness=.4, metallic=0):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (*color, 1)
    mat.use_nodes = True
    node = mat.node_tree.nodes.get('Principled BSDF')
    node.inputs['Base Color'].default_value = (*color, 1)
    node.inputs['Roughness'].default_value = roughness
    node.inputs['Metallic'].default_value = metallic
    return mat

bpy.ops.mesh.primitive_plane_add(size=200)
bpy.context.object.data.materials.append(material('floor', (.3, .32, .35), .65))
for i in range(12):
    theta = i * 2 * math.pi / 12
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=.45,
                                      location=(3 * math.cos(theta), 3 * math.sin(theta), .45))
    obj = bpy.context.object
    obj.data.materials.append(material(f'sphere-{i}', (.15 + .05 * i, .22, .7 - .04 * i), .22, .25))
    for poly in obj.data.polygons:
        poly.use_smooth = True
bpy.ops.mesh.primitive_monkey_add(location=(0, 0, 1.25))
obj = bpy.context.object
obj.data.materials.append(material('suzanne', (.8, .18, .08), .3, .2))
sub = obj.modifiers.new('Subdivision', 'SUBSURF')
sub.levels = 2
for poly in obj.data.polygons:
    poly.use_smooth = True
obj.rotation_euler.z = 0
obj.keyframe_insert(data_path='rotation_euler', frame=1)
obj.rotation_euler.z = math.pi / 2
obj.keyframe_insert(data_path='rotation_euler', frame=24)

for location, energy, size in [((1, -3, 7), 1400, 5), ((-4, 2, 4), 900, 3)]:
    bpy.ops.object.light_add(type='AREA', location=location)
    light = bpy.context.object
    light.data.energy = energy
    light.data.shape = 'DISK'
    light.data.size = size
    light.rotation_euler = (Vector((0, 0, .7)) - light.location).to_track_quat('-Z', 'Y').to_euler()
bpy.ops.object.camera_add(location=(7, -9, 7))
camera = bpy.context.object
camera.rotation_euler = (Vector((0, 0, .8)) - camera.location).to_track_quat('-Z', 'Y').to_euler()
camera.data.lens = 48
scene.camera = camera
scene.render.filepath = '//out/frame_####'
scene.frame_set(1)
bpy.ops.wm.save_as_mainfile(filepath=str(target))
print('ZEROMESH_BENCHMARK_SCENE', target)
