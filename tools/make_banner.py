#!/usr/bin/env python3
"""Draws the HOME Menu banner: the animated 3D model and the flat fallback.

    tools/make_banner.py png  <bold font> meta/banner_3ds.png
    tools/make_banner.py glb  <bold font> scene.glb      (the scene alone)
    tools/make_banner.py cgfx <bold font> <pycgfx dir> meta/banner_3ds.cgfx [scene.glb]

The scene: "3DSync", and a blue and a red arrow running round it along a
rectangle, anticlockwise, one after the other, turning each corner.  The flat
PNG is the same scene at time 0, for `make FLAT_BANNER=1`.

The CGFX is made by writing the scene as glTF and converting it with pycgfx
(https://github.com/skyfloogle/pycgfx), which tools/fetch_pycgfx.sh fetches
at a pinned commit: it has no licence file, so it is never stored here.  What
a banner must do to not crash or freeze the HOME Menu, as found by 3dsDL
(resources/banner/README.md in ankushKun/3dsDL) on a console:

- rigid node animation only, no skinning;
- every mesh names the node it hangs from (pycgfx leaves it blank), and mesh
  and shape names are unique;
- every name lookup resolves through each table's Patricia tree;
- no blended transparency: the wordmark is an alpha cut-out;
- nothing scales to nothing, a few thousand triangles at most, under 512 KiB;
- everything stays inside the area the banner camera shows.

This script does each of those and checks the last ones before writing.  The
camera is fixed at (0, 1, 44.786) looking down -z with a 30 degree vertical
field of view: the plane z = 0 shows x -20..20 and y -11..13, and one unit
is ten pixels of the top screen.

The current banner uses Lato Black (OFL, https://www.latofonts.com; Debian's
fonts-lato).  Needs Pillow, and gltflib for the CGFX.
"""
import json
import math
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

# --- The scene, in banner units ---------------------------------------------
# The rectangle the arrows run along (their centre line).
X0, X1 = -16.0, 16.0
Y0, Y1 = -7.2, 8.5
W, H = X1 - X0, Y1 - Y0
PERIMETER = 2 * (W + H)
STROKE = 1.8          # the shaft is a chain of squares this wide
SPACING = 0.6         # between their centres; a passing corner shows a notch at most half this
HEAD_LEN, HEAD_W = 2.6, 3.6
ARROW_LEN = 0.42 * PERIMETER   # head included; the rest is the gap to the next tail
TURN = 0.9            # the head turns while its base is this close to a corner
LOOP_SECONDS = 6.0    # one full round
TEXT = "3DSync"
TEXT_SIZE = (29.6, 7.4)        # the wordmark quad
TEXT_TEX = (256, 64)
SAFE = (-18.5, 18.5, -9.5, 11.0)  # x min, x max, y min, y max
BLUE = (0.10, 0.16, 1.0, 1.0)
RED = (1.0, 0.10, 0.10, 1.0)
CENTRE_Y = (Y0 + Y1) / 2


def point(s):
    """The centre line at arc length s, anticlockwise from the bottom right
    corner: up the right side, left along the top, down the left side,
    right along the bottom.  Returns (x, y, heading in degrees)."""
    s %= PERIMETER
    if s < H:
        return X1, Y0 + s, 90.0
    s -= H
    if s < W:
        return X1 - s, Y1, 180.0
    s -= W
    if s < H:
        return X0, Y1 - s, 270.0
    s -= H
    return X0 + s, Y0, 0.0


CORNERS = [H, H + W, 2 * H + W, PERIMETER]  # arc length of each corner


def arrows():
    """Each arrow as (colour name, colour, [square offsets], head offset):
    arc lengths at time 0.  Blue's head is just short of the top left
    corner, red's of the bottom right one."""
    out = []
    for name, colour, head in (("Blue", BLUE, H + W - 1.5), ("Red", RED, PERIMETER - 1.5)):
        tail = head - (ARROW_LEN - HEAD_LEN)
        n = int((head - tail) / SPACING) + 1
        squares = [tail + i * (head - tail) / (n - 1) for i in range(n)]
        out.append((name, colour, squares, head))
    return out


def head_heading(s):
    """The head's heading with its base at s: along the side, turning
    linearly through each corner while within TURN of it."""
    s %= PERIMETER
    for c in CORNERS:
        d = s - c if s - c > -PERIMETER / 2 else s - c + PERIMETER
        if -TURN <= d <= TURN:
            before = point(c - 0.01)[2]
            return before + 90.0 * (d + TURN) / (2 * TURN)
    return point(s)[2]


# --- Keyframes ---------------------------------------------------------------
def travel_keys(start):
    """(time, arc length) keys for something at arc length `start` at time 0
    that moves one full round per loop: the ends of the loop, and each corner
    it passes (plus TURN either side, for a head), so linear interpolation
    follows the rectangle exactly."""
    speed = PERIMETER / LOOP_SECONDS
    marks = {0.0, PERIMETER}
    for c in CORNERS:
        for d in (-TURN, 0.0, TURN):
            for k in (0, 1):
                along = (c + d - start) % PERIMETER + k * PERIMETER
                if 0 < along < PERIMETER:
                    marks.add(along)
    return [(a / speed, start + a) for a in sorted(marks)]


# --- The wordmark texture ---------------------------------------------------
def wordmark(font_path):
    """Black letters, outlined in white, with a black drop shadow."""
    S = 4
    w, h = TEXT_TEX
    img = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    size = 60 * S
    while True:
        font = ImageFont.truetype(font_path, size)
        box = d.textbbox((0, 0), TEXT, font=font, stroke_width=3 * S)
        if box[2] - box[0] <= (w - 10) * S and box[3] - box[1] <= (h - 10) * S:
            break
        size -= S
    x = (w * S - (box[2] - box[0])) / 2 - box[0]
    y = (h * S - (box[3] - box[1])) / 2 - box[1] - 1 * S
    d.text((x + 3 * S, y + 3 * S), TEXT, font=font, fill=(0, 0, 0, 255),
           stroke_width=3 * S, stroke_fill=(0, 0, 0, 255))
    d.text((x, y), TEXT, font=font, fill=(0, 0, 0, 255), stroke_width=3 * S,
           stroke_fill=(255, 255, 255, 255))
    return img.resize((w, h), Image.LANCZOS)


# --- The flat banner ----------------------------------------------------------
def flat(font_path, out, t=0.0):
    """The scene t seconds into the loop, at 256 x 128."""
    PX = 6.3  # pixels per unit
    S = 4
    img = Image.new("RGBA", (256 * S, 128 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def px(x, y):
        return ((128 + x * PX) * S, (64 - (y - CENTRE_Y) * PX) * S)

    def colour(c):
        return tuple(int(v * 255) for v in c)

    half = STROKE / 2
    shift = PERIMETER * t / LOOP_SECONDS
    for _, c, squares, head in arrows():
        squares = [q + shift for q in squares]
        head += shift
        s = squares[0]
        while s <= squares[-1]:
            x, y, _ = point(s)
            a, b = px(x - half, y + half), px(x + half, y - half)
            d.rectangle((a, b), fill=colour(c))
            s += 0.05
        x, y, _ = point(head)
        a = math.radians(head_heading(head))
        ca, sa = math.cos(a), math.sin(a)
        tri = [(HEAD_LEN, 0), (0, HEAD_W / 2), (0, -HEAD_W / 2)]
        d.polygon([px(x + u * ca - v * sa, y + u * sa + v * ca) for u, v in tri], fill=colour(c))
    tex = wordmark(font_path)
    tw, th = int(TEXT_SIZE[0] * PX * S), int(TEXT_SIZE[1] * PX * S)
    tex = tex.resize((tw, th), Image.LANCZOS)
    cx, cy = px(0, CENTRE_Y)
    img.alpha_composite(tex, (int(cx - tw / 2), int(cy - th / 2)))
    img = img.resize((256, 128), Image.LANCZOS)
    if out:
        img.save(out)
    return img


# --- The 3D banner -------------------------------------------------------------
class Glb:
    """Just enough of a glTF writer for flat meshes and node animation."""

    def __init__(self):
        self.bin = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "3DSync tools/make_banner.py"},
                    "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [],
                    "materials": [], "accessors": [], "bufferViews": [], "buffers": [],
                    "animations": [{"name": "Loop", "channels": [], "samplers": []}]}

    def accessor(self, values, kind, components, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        flat_values = [v for item in values for v in (item if isinstance(item, tuple) else (item,))]
        data = struct.pack("<%df" % len(flat_values), *flat_values)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.bin += data
        self.doc["bufferViews"].append(view)
        acc = {"bufferView": len(self.doc["bufferViews"]) - 1, "componentType": 5126,
               "count": len(values), "type": kind}
        if kind == "VEC3" or kind == "SCALAR":
            cols = [[v[i] if isinstance(v, tuple) else v for v in values] for i in range(components)]
            acc["min"] = [min(c) for c in cols]
            acc["max"] = [max(c) for c in cols]
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    def material(self, name, colour, texture=None):
        m = {"name": name, "doubleSided": True,
             "pbrMetallicRoughness": {"baseColorFactor": list(colour), "metallicFactor": 0.0,
                                      "roughnessFactor": 1.0}}
        if texture is not None:
            m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": texture}
            m["alphaMode"] = "MASK"
            m["alphaCutoff"] = 0.5
        self.doc["materials"].append(m)
        return len(self.doc["materials"]) - 1

    def node(self, name, mesh, translation):
        self.doc["meshes"].append({"name": name + "~m", "primitives": [mesh]})
        self.doc["nodes"].append({"name": name, "mesh": len(self.doc["meshes"]) - 1,
                                  "translation": list(translation)})
        idx = len(self.doc["nodes"]) - 1
        self.doc["scenes"][0]["nodes"].append(idx)
        return idx

    def animate(self, node, path, times, values):
        anim = self.doc["animations"][0]
        kind = {"translation": "VEC3", "rotation": "VEC4"}[path]
        inp = self.accessor(times, "SCALAR", 1)
        out = self.accessor(values, kind, 3 if kind == "VEC3" else 4)
        anim["samplers"].append({"input": inp, "output": out, "interpolation": "LINEAR"})
        anim["channels"].append({"sampler": len(anim["samplers"]) - 1,
                                 "target": {"node": node, "path": path}})

    def image(self, png_bytes):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.doc["bufferViews"].append({"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(png_bytes)})
        self.bin += png_bytes
        self.doc.setdefault("images", []).append({"name": "Wordmark", "mimeType": "image/png",
                                                   "bufferView": len(self.doc["bufferViews"]) - 1})
        self.doc.setdefault("samplers", []).append({"magFilter": 9729, "minFilter": 9729,
                                                    "wrapS": 33071, "wrapT": 33071})
        self.doc.setdefault("textures", []).append({"source": 0, "sampler": 0})
        return 0

    def write(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.doc["buffers"] = [{"byteLength": len(self.bin)}]
        js = json.dumps(self.doc, separators=(",", ":")).encode()
        while len(js) % 4:
            js += b" "
        total = 12 + 8 + len(js) + 8 + len(self.bin)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, total))
            f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
            f.write(struct.pack("<II", len(self.bin), 0x004E4942) + bytes(self.bin))


def quad(g, w, h, material, uv=False):
    """Two triangles in the z = 0 plane, centred on the node, facing +z."""
    pos = [(-w / 2, -h / 2, 0.0), (w / 2, -h / 2, 0.0), (w / 2, h / 2, 0.0), (-w / 2, h / 2, 0.0)]
    tris = [0, 1, 2, 0, 2, 3]
    attrs = {"POSITION": g.accessor([pos[i] for i in tris], "VEC3", 3, 34962),
             "NORMAL": g.accessor([(0.0, 0.0, 1.0)] * 6, "VEC3", 3, 34962)}
    if uv:
        st = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
        attrs["TEXCOORD_0"] = g.accessor([st[i] for i in tris], "VEC2", 2, 34962)
    return {"attributes": attrs, "material": material}


def head_mesh(g, material):
    """The arrowhead pointing along +x, its base centred on the node."""
    pos = [(HEAD_LEN, 0.0, 0.0), (0.0, HEAD_W / 2, 0.0), (0.0, -HEAD_W / 2, 0.0)]
    return {"attributes": {"POSITION": g.accessor(pos, "VEC3", 3, 34962),
                           "NORMAL": g.accessor([(0.0, 0.0, 1.0)] * 3, "VEC3", 3, 34962)},
            "material": material}


def check_safe(points):
    for x, y in points:
        if not (SAFE[0] <= x <= SAFE[1] and SAFE[2] <= y <= SAFE[3]):
            sys.exit("a part leaves the safe area at (%.1f, %.1f)" % (x, y))


def z_quat(degrees):
    a = math.radians(degrees) / 2
    return (0.0, 0.0, math.sin(a), math.cos(a))


def build_glb(font_path, out):
    import io
    g = Glb()
    buf = io.BytesIO()
    wordmark(font_path).save(buf, "PNG")
    tex = g.image(buf.getvalue())
    text_mat = g.material("Wordmark", (1.0, 1.0, 1.0, 1.0), tex)
    g.node("Wordmark", quad(g, TEXT_SIZE[0], TEXT_SIZE[1], text_mat, uv=True), (0.0, CENTRE_Y, 0.2))

    for name, colour, squares, head in arrows():
        mat = g.material(name, colour)
        for i, s in enumerate(squares):
            x, y, _ = point(s)
            n = g.node("%s%02d" % (name, i), quad(g, STROKE, STROKE, mat), (x, y, 0.0))
            keys = travel_keys(s)
            g.animate(n, "translation", [t for t, _ in keys],
                      [point(a)[:2] + (0.0,) for _, a in keys])
        x, y, _ = point(head)
        n = g.node(name + "Head", head_mesh(g, mat), (x, y, 0.01))
        keys = travel_keys(head)
        g.animate(n, "translation", [t for t, _ in keys], [point(a)[:2] + (0.01,) for _, a in keys])
        g.animate(n, "rotation", [t for t, _ in keys], [z_quat(head_heading(a)) for _, a in keys])

    # Inside the safe area in every frame: each square's and each head's
    # corners, sampled through the loop the way linear keys move them.
    for _, _, squares, head in arrows():
        for k in range(600):
            t = PERIMETER * k / 600
            for sq in squares:
                x, y, _ = point(sq + t)
                corners = [(x - STROKE / 2, y - STROKE / 2), (x + STROKE / 2, y + STROKE / 2)]
                check_safe(corners)
            x, y, _ = point(head + t)
            a = math.radians(head_heading(head + t))
            ca, sa = math.cos(a), math.sin(a)
            check_safe([(x + u * ca - v * sa, y + u * sa + v * ca)
                        for u, v in ((HEAD_LEN, 0), (0, HEAD_W / 2), (0, -HEAD_W / 2))])
    for x, y in ((-TEXT_SIZE[0] / 2, CENTRE_Y - TEXT_SIZE[1] / 2), (TEXT_SIZE[0] / 2, CENTRE_Y + TEXT_SIZE[1] / 2)):
        if not (SAFE[0] <= x <= SAFE[1] and SAFE[2] <= y <= SAFE[3]):
            sys.exit("the wordmark leaves the safe area")
    g.write(out)
    return len(g.doc["nodes"])


# --- glTF -> CGFX ----------------------------------------------------------------
def lookups_fail(dict_info):
    """Names a console lookup in this table would not find: walks the
    Patricia tree from the root the way the HOME Menu does."""
    nodes = dict_info.dict.nodes
    if len(nodes) <= 1:
        return []
    width = max(len(n.get_name()) for n in nodes[1:])

    def bit(name, index):
        name = name.ljust(width, "\0")
        return index // 8 < len(name) and (ord(name[index // 8]) >> (index & 7)) & 1

    def refbit(i):
        return math.inf if i == 0 else nodes[i].refbit

    failed = []
    for n in nodes[1:]:
        name = n.get_name()
        current, nxt = 0, nodes[0].left_index
        while refbit(current) > refbit(nxt):
            current = nxt
            nxt = nodes[current].right_index if bit(name, nodes[current].refbit) else nodes[current].left_index
        if nodes[nxt].get_name() != name:
            failed.append(name)
    return failed


def convert(pycgfx_dir, glb, out):
    sys.path.insert(0, pycgfx_dir)
    import gltflib
    import main as pycgfx

    cgfx = pycgfx.convert_gltf(gltflib.GLTF.load(glb, load_file_resources=True))
    model = cgfx.data.models["COMMON"]

    # Every mesh names the bone it hangs from; mesh and shape names unique.
    meshes = model.meshes.data.contents
    shapes = model.shapes.data.contents
    bones = set(model.skeleton.bones)
    for mesh in meshes:
        bone = mesh.name.split("~m")[0]
        if bone not in bones:
            sys.exit("mesh %s has no bone %s" % (mesh.name, bone))
        mesh.mesh_node_name = bone
        shapes[mesh.shape_index].name = mesh.name
    names = [m.name for m in meshes]
    if len(set(names)) != len(names):
        sys.exit("mesh names repeat")

    # Every name table in the file, rebuilt and walked the way the console
    # looks names up: found by walking the objects, so none is missed.
    from cgfx.dict import DictInfo
    seen, checked = set(), 0

    def walk(obj, where):
        nonlocal checked
        if id(obj) in seen or obj is None or isinstance(obj, (str, bytes, int, float, bool)):
            return
        seen.add(id(obj))
        if isinstance(obj, DictInfo):
            if obj.dict.len():
                obj.dict.regenerate()
                bad = lookups_fail(obj)
                if bad:
                    sys.exit("%s: lookups would not find %s" % (where, ", ".join(bad)))
                checked += 1
            for n in obj.dict.nodes[1:]:
                walk(n.content, where + "/" + n.get_name())
            return
        if isinstance(obj, (list, tuple)):
            for i, item in enumerate(obj):
                walk(item, where)
            return
        if hasattr(obj, "__dict__"):
            for k, v in vars(obj).items():
                if k not in ("owner", "parent", "child", "next_sibling", "prev_sibling"):
                    walk(v, where + "." + k)

    walk(cgfx.data, "cgfx")

    data = pycgfx.write(cgfx)
    if len(data) > 0x80000:
        sys.exit("CGFX is %d bytes, over the 512 KiB the HOME Menu takes" % len(data))
    with open(out, "wb") as f:
        f.write(data)
    return len(data), checked


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "png":
        flat(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "glb":
        print("%s: %d nodes" % (sys.argv[3], build_glb(sys.argv[2], sys.argv[3])))
    elif len(sys.argv) in (5, 6) and sys.argv[1] == "cgfx":
        glb = sys.argv[5] if len(sys.argv) == 6 else os.path.splitext(sys.argv[4])[0] + ".glb"
        nodes = build_glb(sys.argv[2], glb)
        size, tables = convert(sys.argv[3], glb, sys.argv[4])
        print("%s: %d nodes, %d bytes, every lookup resolves in %d tables" % (sys.argv[4], nodes, size, tables))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
