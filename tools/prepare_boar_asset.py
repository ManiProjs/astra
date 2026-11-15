#!/usr/bin/env python3
"""Build a dense, skinned boar from CC0 geometry and CC0 rig/animation sources.

Run with Blender 5.2:
  Blender -b --python tools/prepare_boar_asset.py

The dense Forest Boar geometry receives interpolated weights from the 16-bone
WildBoar rig. The rig's four animation clips are preserved. Weight transfer is
performed in normalized boar-local bounds and the rest skeleton is remapped to
the detailed mesh's bounds before export.
"""
from __future__ import annotations

import random
from array import array
from pathlib import Path

import bpy
from mathutils import Vector
from mathutils.kdtree import KDTree

ROOT = Path(__file__).resolve().parents[1]
GEOMETRY_SOURCE = ROOT / 'assets/characters/source/3DAssets-Forest-Boar-source.glb'
RIG_SOURCE = ROOT / 'assets/characters/source/Gobkit-WildBoar-rig.glb'
OUTPUT = ROOT / 'assets/characters/AstraBoar.glb'
TEXTURE = ROOT / 'assets/characters/AstraBoarSkin.png'


def bounds(vertices):
    points = [v.co for v in vertices]
    lo = Vector(tuple(min(p[i] for p in points) for i in range(3)))
    hi = Vector(tuple(max(p[i] for p in points) for i in range(3)))
    return lo, hi


def map_between_bounds(point, source_lo, source_size, target_lo, target_size):
    normalized = Vector(tuple((point[i] - source_lo[i]) / max(source_size[i], 1.0e-8)
                              for i in range(3)))
    return Vector(tuple(target_lo[i] + normalized[i] * target_size[i] for i in range(3)))


def create_texture():
    random.seed(71)
    size = 1024
    pixels = array('f', [0.0]) * (size * size * 4)
    coarse = [[random.random() for _ in range(64)] for _ in range(64)]
    for y in range(size):
        gy = y * 63 / (size - 1); y0 = int(gy); fy = gy - y0
        for x in range(size):
            gx = x * 63 / (size - 1); x0 = int(gx); fx = gx - x0
            a = coarse[y0][x0] * (1 - fx) + coarse[y0][min(63, x0 + 1)] * fx
            b = coarse[min(63, y0 + 1)][x0] * (1 - fx) + coarse[min(63, y0 + 1)][min(63, x0 + 1)] * fx
            n = (a * (1 - fy) + b * fy) * 0.78 + random.random() * 0.22
            v = max(0.0, min(1.0, (42 + n * 83) / 255.0))
            i = (y * size + x) * 4
            pixels[i:i + 4] = array('f', (v, v * 0.72, v * 0.53, 1.0))
    image = bpy.data.images.new('AstraBoarSkin', width=size, height=size, alpha=True)
    image.pixels.foreach_set(pixels)
    image.pack()
    image.filepath_raw = str(TEXTURE)
    image.file_format = 'PNG'
    image.save()
    return image


def make_material(image):
    material = bpy.data.materials.new('Astra Boar | textured hide')
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    bsdf = nodes.get('Principled BSDF')
    bsdf.inputs['Roughness'].default_value = 0.91
    tex = nodes.new('ShaderNodeTexImage')
    tex.image = image
    links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
    return material


def import_detailed_geometry():
    bpy.ops.import_scene.gltf(filepath=str(GEOMETRY_SOURCE))
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == 'MESH']
    if not meshes:
        raise RuntimeError('Detailed CC0 boar source contains no meshes')
    for obj in meshes:
        bpy.context.view_layer.objects.active = obj
        if obj.parent:
            bpy.ops.object.parent_clear(type='CLEAR_KEEP_TRANSFORM')
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    bpy.ops.object.select_all(action='DESELECT')
    for obj in meshes:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.object.join()
    target = bpy.context.object
    target.name = 'AstraBoar'
    for obj in list(bpy.context.scene.objects):
        if obj != target and obj.type != 'MESH':
            bpy.data.objects.remove(obj, do_unlink=True)
    for action in list(bpy.data.actions):
        bpy.data.actions.remove(action)
    # Join source islands, then increase geometric sampling while retaining the
    # authored body, head, ears, tusks, snout, tail and bristle silhouettes.
    remesh = target.modifiers.new('Weld close anatomical seams', 'REMESH')
    remesh.mode = 'VOXEL'
    remesh.voxel_size = 0.006
    bpy.ops.object.modifier_apply(modifier=remesh.name)
    smooth = target.modifiers.new('Relax remesh surface', 'SMOOTH')
    smooth.factor = 0.25
    smooth.iterations = 2
    bpy.ops.object.modifier_apply(modifier=smooth.name)
    subdiv = target.modifiers.new('High density surface', 'SUBSURF')
    subdiv.subdivision_type = 'CATMULL_CLARK'
    subdiv.levels = 1
    bpy.ops.object.modifier_apply(modifier=subdiv.name)
    for face in target.data.polygons:
        face.use_smooth = True
    return target


def import_rig():
    bpy.ops.import_scene.gltf(filepath=str(RIG_SOURCE))
    armatures = [obj for obj in bpy.context.scene.objects if obj.type == 'ARMATURE']
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == 'MESH']
    if len(armatures) != 1:
        raise RuntimeError(f'Expected one source armature, got {len(armatures)}')
    source_mesh = next((obj for obj in meshes if obj.find_armature() is not None), None)
    if source_mesh is None:
        raise RuntimeError('Rig source contains no skinned mesh')
    return armatures[0], source_mesh


def transfer_weights(target, source_mesh, armature):
    src_lo, src_hi = bounds(source_mesh.data.vertices)
    dst_lo, dst_hi = bounds(target.data.vertices)
    src_size = src_hi - src_lo
    dst_size = dst_hi - dst_lo
    tree = KDTree(len(source_mesh.data.vertices))
    for index, vertex in enumerate(source_mesh.data.vertices):
        tree.insert(vertex.co, index)
    tree.balance()
    source_group_names = [group.name for group in source_mesh.vertex_groups]
    target_groups = {name: target.vertex_groups.new(name=name) for name in source_group_names}
    source_weights = []
    for vertex in source_mesh.data.vertices:
        source_weights.append({group.group: group.weight for group in vertex.groups})
    max_projection = 0.0
    for index, vertex in enumerate(target.data.vertices):
        rig_point = map_between_bounds(vertex.co, dst_lo, dst_size, src_lo, src_size)
        nearest = tree.find_n(rig_point, 4)
        weighted = {}
        total = 0.0
        for _co, source_index, distance in nearest:
            influence = 1.0 / max(distance * distance, 1.0e-8)
            total += influence
            for group_index, weight in source_weights[source_index].items():
                weighted[group_index] = weighted.get(group_index, 0.0) + influence * weight
            max_projection = max(max_projection, distance)
        if total <= 0.0:
            continue
        normalized = [(group, weight / total) for group, weight in weighted.items()]
        sum_weights = sum(weight for _, weight in normalized)
        if sum_weights <= 1.0e-8:
            continue
        for group_index, weight in normalized:
            if weight > 1.0e-5:
                name = source_group_names[group_index]
                target_groups[name].add([index], weight / sum_weights, 'REPLACE')

    # Move the skeleton's rest joints into the detailed geometry's local bounds.
    bpy.context.view_layer.objects.active = armature
    bpy.ops.object.mode_set(mode='EDIT')
    for bone in armature.data.edit_bones:
        bone.head = map_between_bounds(bone.head, src_lo, src_size, dst_lo, dst_size)
        bone.tail = map_between_bounds(bone.tail, src_lo, src_size, dst_lo, dst_size)
        if (bone.tail - bone.head).length < 0.005:
            bone.tail.z += 0.01
    bpy.ops.object.mode_set(mode='OBJECT')
    target.parent = None
    modifier = target.modifiers.new('AstraBoarRig', 'ARMATURE')
    modifier.object = armature
    modifier.use_vertex_groups = True
    armature.name = 'AstraBoarRig'
    target.parent = armature
    return len(armature.data.bones), max_projection


def main():
    if not GEOMETRY_SOURCE.exists() or not RIG_SOURCE.exists():
        raise RuntimeError('Missing source GLB. See asset source/license notes.')
    bpy.ops.wm.read_factory_settings(use_empty=True)
    target = import_detailed_geometry()
    armature, source_mesh = import_rig()
    bone_count, max_weight_distance = transfer_weights(target, source_mesh, armature)
    # Remove the low-poly donor mesh and its unused accessories; keep its rig/actions.
    bpy.data.objects.remove(source_mesh, do_unlink=True)
    for obj in list(bpy.context.scene.objects):
        if obj.type == 'MESH' and obj != target:
            bpy.data.objects.remove(obj, do_unlink=True)
    image = create_texture()
    material = make_material(image)
    target.data.materials.clear()
    target.data.materials.append(material)
    bpy.context.view_layer.objects.active = target
    target.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(island_margin=0.015)
    bpy.ops.object.mode_set(mode='OBJECT')
    for action in bpy.data.actions:
        print('ANIMATION', action.name, tuple(action.frame_range))
    if len(bpy.data.actions) < 4:
        raise RuntimeError(f'Expected four source animation clips, got {len(bpy.data.actions)}')
    armature.select_set(True)
    bpy.context.view_layer.objects.active = target
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(filepath=str(OUTPUT), export_format='GLB', export_animations=True,
        export_skins=True, export_texcoords=True, export_normals=True, export_materials='EXPORT',
        export_image_format='AUTO', export_yup=True)
    triangles = sum(len(poly.vertices) - 2 for poly in target.data.polygons)
    used_groups = {group.group for vertex in target.data.vertices for group in vertex.groups}
    used_bones = len({target.vertex_groups[index].name for index in used_groups})
    report = (f'Geometry source: 3DAssets.dev Forest Boar (CC0 1.0)\n'
              f'Rig/animation source: Gobkit WildBoar (CC0 1.0)\n'
              f'Vertices: {len(target.data.vertices):,}\nTriangles: {triangles:,}\n'
              f'Rig bones: {bone_count} ({used_bones} have transferred vertex weights)\n'
              f'Animations: {len(bpy.data.actions)}\n'
              f'Maximum nearest-weight transfer distance in rig space: {max_weight_distance:.6f}\n'
              f'Texture: 1024x1024 generated hide\n')
    (ROOT / 'assets/characters/AstraBoar-report.txt').write_text(report)
    print(report)
    print(f'Wrote {OUTPUT} ({OUTPUT.stat().st_size:,} bytes)')


if __name__ == '__main__':
    main()
