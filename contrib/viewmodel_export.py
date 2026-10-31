"""Makes a ReSkate viewmodel (reskate.viewmodel) from a rigged, animated model Blender can open.

Run it with Blender, which reads the model (FBX, glTF, .blend) and does the maths:

    blender -b --python contrib/viewmodel_export.py -- <model> <out folder> [options]

    --camera-bone NAME   the bone the view is from (default Head_Cam; without one, the scene's origin
                         looking along -Y with +Z up, Blender's front view)
    --textures DIR       where the model's pictures are, when the paths saved in it are wrong
    --size N             the largest a picture is written (default 2048; pictures are square or smaller)
    --name NAME          the file's name (default viewmodel)

It writes <out>/<name>.rsvm and three pictures for each material: <material>_albedo.png (colour, sRGB),
<material>_normal.png (tangent space, +Y up as Blender has it) and <material>_surface.png (R roughness,
G ambient occlusion, B specular). The layout of .rsvm is in Extension/UI/Overlay/viewmodel_file.h: the
skin matrices of every frame of every action, already in the view's space (x right, y up, z forward,
metres), so ReSkate needs no rig maths of its own.
"""

import math
import os
import re
import struct
import sys

import bpy
import mathutils
import numpy

MAGIC = b"RSVM"
VERSION = 1
MAX_BONES = 128


def arguments():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    options = {"camera_bone": "Head_Cam", "textures": None, "size": 2048, "name": "viewmodel"}
    positional = []
    i = 0
    while i < len(argv):
        word = argv[i]
        if word.startswith("--"):
            key = word[2:].replace("-", "_")
            if key not in options or i + 1 >= len(argv):
                sys.exit(f"unknown option {word}, or no value after it")
            options[key] = int(argv[i + 1]) if key == "size" else argv[i + 1]
            i += 2
        else:
            positional.append(word)
            i += 1
    if len(positional) != 2:
        sys.exit("usage: blender -b --python viewmodel_export.py -- <model> <out folder> [options]")
    return positional[0], positional[1], options


def load(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    lower = path.lower()
    if lower.endswith(".fbx"):
        bpy.ops.import_scene.fbx(filepath=path)
    elif lower.endswith((".glb", ".gltf")):
        bpy.ops.import_scene.gltf(filepath=path)
    elif lower.endswith(".blend"):
        bpy.ops.wm.open_mainfile(filepath=path)
    else:
        sys.exit("the model must be .fbx, .glb, .gltf or .blend")


# The view's axes from Blender's: x right, y up, z forward. Blender's front view looks along -Y with +Z up,
# so right is -X; a camera bone (Blender bones point along their Y) is taken the same way, its Y up and
# its Z forward (as Head_Cam is rigged), so right is its -X. A reflection, which is why tangents' signs
# flip below.
AXES = mathutils.Matrix(((-1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1)))
FRONT_VIEW = mathutils.Matrix(((-1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))


def view_from(camera):
    """The matrix from Blender's world into the view, for the camera bone's matrix (or None)."""
    if camera is None:
        return FRONT_VIEW
    return AXES @ camera.inverted()


def rows(matrix):
    """A 4x4 as the 3x4 the file keeps: its first three rows."""
    return [matrix[r][c] for r in range(3) for c in range(4)]


def short(text):
    data = text.encode("utf-8")[:255]
    return struct.pack("<B", len(data)) + data


def main():
    model, out, options = arguments()
    out = os.path.abspath(out)
    load(model)
    os.makedirs(out, exist_ok=True)
    rigs = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    if len(rigs) != 1:
        sys.exit(f"the model needs exactly one armature, and has {len(rigs)}")
    rig = rigs[0]
    bones = list(rig.data.bones)
    if len(bones) > MAX_BONES:
        sys.exit(f"{len(bones)} bones: a viewmodel has at most {MAX_BONES}")
    index_of = {b.name: i for i, b in enumerate(bones)}
    camera_name = options["camera_bone"]
    if camera_name not in index_of:
        print(f"no bone {camera_name}: the view is Blender's front view from the origin")
        camera_name = None

    def camera_now():
        if camera_name is None:
            return None
        return rig.matrix_world @ rig.pose.bones[camera_name].matrix

    # The rest pose: what the vertices are given in.
    rig.data.pose_position = "REST"
    bpy.context.view_layer.update()
    rest_camera = None if camera_name is None else rig.matrix_world @ rig.data.bones[camera_name].matrix_local
    rest_view = view_from(rest_camera)
    binds = [rest_view @ rig.matrix_world @ b.matrix_local for b in bones]

    meshes = [o for o in bpy.data.objects if o.type == "MESH" and any(m.type == "ARMATURE" and m.object == rig for m in o.modifiers)]
    if not meshes:
        sys.exit("no mesh is skinned to the armature")
    materials, parts, vertices, indices = [], [], [], []
    material_index = {}
    for obj in meshes:
        mesh = obj.data
        uv = mesh.uv_layers.active
        if uv is None:
            sys.exit(f"{obj.name} has no UV map")
        mesh.calc_tangents(uvmap=uv.name)
        to_view = rest_view @ obj.matrix_world
        normal_to_view = to_view.to_3x3().inverted().transposed()
        group_bone = {g.index: index_of.get(g.name) for g in obj.vertex_groups}
        weights_of = []
        for v in mesh.vertices:
            found = [(g.weight, group_bone[g.group]) for g in v.groups if group_bone.get(g.group) is not None and g.weight > 0]
            found.sort(reverse=True)
            found = found[:4]
            total = sum(w for w, _ in found)
            if total <= 0:
                sys.exit(f"{obj.name}: vertex {v.index} is weighted to no bone")
            quantised = [int(round(w / total * 255)) for w, _ in found]
            quantised[0] += 255 - sum(quantised)
            joints = [b for _, b in found] + [0] * (4 - len(found))
            weights_of.append((joints, quantised + [0] * (4 - len(found))))
        corner_normals = mesh.corner_normals
        made = {}
        # One part per material slot used.
        by_material = {}
        mesh.calc_loop_triangles()
        for tri in mesh.loop_triangles:
            by_material.setdefault(tri.material_index, []).append(tri)
        for slot, tris in sorted(by_material.items()):
            material = obj.material_slots[slot].material if slot < len(obj.material_slots) else None
            key = material.name if material else "default"
            if key not in material_index:
                material_index[key] = len(materials)
                materials.append(material)
            first = len(indices)
            for tri in tris:
                for loop_index in tri.loops:
                    loop = mesh.loops[loop_index]
                    position = to_view @ mesh.vertices[loop.vertex_index].co
                    normal = (normal_to_view @ corner_normals[loop_index].vector).normalized()
                    tangent = (to_view.to_3x3() @ loop.tangent).normalized()
                    sign = -loop.bitangent_sign  # the reflection into the view's axes
                    u, v = uv.data[loop_index].uv
                    joints, weights = weights_of[loop.vertex_index]
                    record = (tuple(round(c, 5) for c in position), tuple(round(c, 4) for c in normal),
                              tuple(round(c, 4) for c in tangent), sign, round(u, 5), round(1.0 - v, 5), tuple(joints), tuple(weights))
                    found = made.get(record)
                    if found is None:
                        found = len(vertices)
                        made[record] = found
                        vertices.append(record)
                    indices.append(found)
            parts.append((material_index[key], first, len(indices) - first))
        mesh.free_tangents()

    # Every action, a frame at a time: the skin matrix of every bone, in the view of that frame's camera.
    rig.data.pose_position = "POSE"
    scene = bpy.context.scene
    fps = scene.render.fps / scene.render.fps_base
    if rig.animation_data is None:
        rig.animation_data_create()
    clips = []
    inverse_binds = [b.inverted() for b in binds]
    for action in bpy.data.actions:
        rig.animation_data.action = action
        if hasattr(rig.animation_data, "action_slot") and getattr(action, "slots", None):
            rig.animation_data.action_slot = action.slots[0]
        start, end = (int(math.floor(action.frame_range[0])), int(math.ceil(action.frame_range[1])))
        frames = []
        for frame in range(start, end + 1):
            scene.frame_set(frame)
            view = view_from(camera_now())
            frames.append([rows(view @ rig.matrix_world @ rig.pose.bones[b.name].matrix @ inverse_binds[i]) for i, b in enumerate(bones)])
        name = action.name.split("|")[-1]
        clips.append((name, fps, frames))
        print(f"clip {name}: {len(frames)} frames")

    textures = write_textures(materials, out, options)

    with open(os.path.join(out, options["name"] + ".rsvm"), "wb") as f:
        f.write(MAGIC + struct.pack("<I", VERSION))
        f.write(struct.pack("<6I", len(bones), len(materials), len(parts), len(vertices), len(indices), len(clips)))
        for i, b in enumerate(bones):
            f.write(short(b.name) + struct.pack("<h", index_of[b.parent.name] if b.parent else -1))
            f.write(struct.pack("<12f", *rows(binds[i])))
        for material, (albedo, normal, surface, roughness, specular) in zip(materials, textures):
            f.write(short(material.name if material else "default") + short(albedo) + short(normal) + short(surface))
            f.write(struct.pack("<2f", roughness, specular))
        for part in parts:
            f.write(struct.pack("<3I", *part))
        for position, normal, tangent, sign, u, v, joints, weights in vertices:
            f.write(struct.pack("<3f3f4f2f4B4B", *position, *normal, *tangent, sign, u, v, *joints, *weights))
        f.write(numpy.asarray(indices, dtype="<u4").tobytes())
        for name, rate, frames in clips:
            f.write(short(name) + struct.pack("<fI", rate, len(frames)))
            f.write(numpy.asarray(frames, dtype="<f4").tobytes())
    print(f"wrote {options['name']}.rsvm: {len(bones)} bones, {len(materials)} materials, {len(vertices)} vertices, "
          f"{len(indices) // 3} triangles, {len(clips)} clips")


# ---- pictures --------------------------------------------------------------------------------------

def image_into(node_tree, socket_names):
    """The picture that feeds one of the principled shader's inputs, through a normal map node if any."""
    if node_tree is None:
        return None
    for node in node_tree.nodes:
        if node.type != "TEX_IMAGE" or node.image is None:
            continue
        for output in node.outputs:
            for link in output.links:
                to = link.to_node
                if to.type == "BSDF_PRINCIPLED" and link.to_socket.name in socket_names:
                    return node.image
                if to.type == "NORMAL_MAP" and "Normal" in socket_names:
                    return node.image
    return None


def principled_value(material, name, fallback):
    if material is None or material.node_tree is None:
        return fallback
    for node in material.node_tree.nodes:
        if node.type == "BSDF_PRINCIPLED" and name in node.inputs:
            value = node.inputs[name].default_value
            return float(value) if isinstance(value, float) else fallback
    return fallback


def find_file(image, folder):
    """The picture's file: where the model says, or by its name in --textures."""
    if image is None:
        return None
    path = bpy.path.abspath(image.filepath)
    if os.path.isfile(path):
        return path
    if folder:
        candidate = os.path.join(folder, os.path.basename(path.replace("\\", "/")))
        if os.path.isfile(candidate):
            return candidate
    return None


def sibling(path, words):
    """A picture beside `path` with the same name up to its last word, ending in one of `words` (armColor.png
    -> armAO.png; x_Diffuse.png -> x_Glossiness.png)."""
    if path is None:
        return None
    folder, name = os.path.split(path)
    stem = os.path.splitext(name)[0]
    prefix = re.sub(r"(?i)(color|colour|diffuse|albedo|basecolor|base_color)$", "", stem)
    for file in os.listdir(folder):
        other = os.path.splitext(file)[0]
        if other.lower().startswith(prefix.lower()) and other[len(prefix):].lower() in words:
            return os.path.join(folder, file)
    return None


def read_pixels(path, size):
    """The picture at `path` as rows top first, RGBA floats 0..1 as stored (no colour conversion), at most
    size x size."""
    image = bpy.data.images.load(path, check_existing=False)
    image.colorspace_settings.name = "Non-Color"
    width, height = image.size
    scale = min(1.0, size / max(width, height))
    if scale < 1.0:
        image.scale(max(1, int(width * scale)), max(1, int(height * scale)))
        width, height = image.size
    pixels = numpy.empty(width * height * 4, dtype=numpy.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    return pixels.reshape(height, width, 4)[::-1]


def write_png(pixels, path):
    height, width = pixels.shape[:2]
    image = bpy.data.images.new(os.path.basename(path), width, height, alpha=True)
    image.colorspace_settings.name = "Non-Color"
    image.pixels.foreach_set(numpy.ascontiguousarray(pixels[::-1]).ravel())
    image.filepath_raw = path
    image.file_format = "PNG"
    image.save()
    bpy.data.images.remove(image)


def safe_name(text):
    return re.sub(r"[^A-Za-z0-9_-]", "_", text) or "material"


def write_textures(materials, out, options):
    size, folder = options["size"], options["textures"]
    result = []
    used = set()
    for material in materials:
        name = safe_name(material.name if material else "default")
        while name in used:
            name += "_"
        used.add(name)
        tree = material.node_tree if material else None
        albedo_path = find_file(image_into(tree, {"Base Color"}), folder)
        normal_path = find_file(image_into(tree, {"Normal"}), folder)
        rough_path = find_file(image_into(tree, {"Roughness"}), folder)
        spec_path = find_file(image_into(tree, {"Specular IOR Level", "Specular"}), folder)
        ao_path = sibling(albedo_path, {"ao", "_ao", "ambientocclusion", "_ambientocclusion", "occlusion", "_occlusion"})
        gloss_path = None if rough_path else sibling(albedo_path, {"glossiness", "_glossiness", "gloss", "_gloss"})
        roughness = principled_value(material, "Roughness", 0.5)
        specular = principled_value(material, "Specular IOR Level", 0.5)
        files = ["", "", ""]
        if albedo_path:
            files[0] = f"{name}_albedo.png"
            write_png(read_pixels(albedo_path, size), os.path.join(out, files[0]))
        if normal_path:
            files[1] = f"{name}_normal.png"
            pixels = read_pixels(normal_path, size)
            pixels[..., 3] = 1.0
            write_png(pixels, os.path.join(out, files[1]))
        if rough_path or gloss_path or ao_path or spec_path:
            shape = None
            channels = {}
            for key, path in (("rough", rough_path), ("gloss", gloss_path), ("ao", ao_path), ("spec", spec_path)):
                if path:
                    channels[key] = read_pixels(path, size)
                    shape = shape or channels[key].shape
            def plane(key, fallback):
                if key not in channels:
                    return numpy.full(shape[:2], fallback, dtype=numpy.float32)
                picture = channels[key]
                if picture.shape != shape:  # sizes differ: nearest, which is enough for these
                    ys = (numpy.arange(shape[0]) * picture.shape[0] // shape[0])
                    xs = (numpy.arange(shape[1]) * picture.shape[1] // shape[1])
                    picture = picture[ys][:, xs]
                return picture[..., 0]
            rough = plane("rough", roughness) if rough_path or not gloss_path else 1.0 - plane("gloss", 1.0 - roughness)
            surface = numpy.stack([rough, plane("ao", 1.0), plane("spec", specular), numpy.ones(shape[:2], dtype=numpy.float32)], axis=-1)
            files[2] = f"{name}_surface.png"
            write_png(surface, os.path.join(out, files[2]))
        print(f"material {name}: albedo {albedo_path}, normal {normal_path}, roughness {rough_path or gloss_path}, ao {ao_path}, specular {spec_path}")
        result.append((files[0], files[1], files[2], roughness, specular))
    return result


main()
