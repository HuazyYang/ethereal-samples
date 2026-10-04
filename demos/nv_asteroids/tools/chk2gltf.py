"""Convert NVDACHNK (.chk) meshlet files to glTF 2.0 binary (.glb) or Wavefront OBJ.

Default output keeps normals/tangents as normalized int8 (KHR_mesh_quantization), copying the
original packed data; --float decodes them to float32 for maximum viewer compatibility.
Diffuse textures are resolved through Clustered/materials.json and referenced by relative URI.
"""
import argparse
import json
import os
import re
import struct
import sys

import numpy as np

from chk import ChunkFile, MESHSET_MESHLETS


def sn8(data, comps=3):
    """Decode packed snorm8x4 vectors to float32 (n, comps)."""
    v = np.frombuffer(data, dtype=np.int8).reshape(-1, 4)[:, :comps]
    return np.maximum(v.astype(np.float32) / 127.0, -1.0)


def load_materials(media_root):
    path = os.path.join(media_root, "Clustered", "materials.json")
    if not os.path.exists(path):
        return {}
    text = open(path, encoding="utf-8").read()
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r",(\s*[}\]])", r"\1", text)
    return json.loads(text)


def tangent_signs(ms, n):
    """Tangent w: +1 if (N x T) . B >= 0, else -1. The file stores w=0."""
    N, T, B = sn8(ms["normal"].data), sn8(ms["tangent"].data), sn8(ms["bitangent"].data)
    return np.einsum("ij,ij->i", np.cross(N, T), B) >= 0


class GlbBuilder:
    def __init__(self):
        self.bin = bytearray()
        self.views, self.accessors = [], []

    def add(self, data, target=None, stride=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            view["target"] = target
        if stride:
            view["byteStride"] = stride
        self.bin += data
        self.views.append(view)
        return len(self.views) - 1

    def accessor(self, view, ctype, count, atype, normalized=False, minv=None, maxv=None):
        a = {"bufferView": view, "componentType": ctype, "count": count, "type": atype}
        if normalized:
            a["normalized"] = True
        if minv is not None:
            a["min"], a["max"] = list(minv), list(maxv)
        self.accessors.append(a)
        return len(self.accessors) - 1


def convert(path, out_path, media_root, materials_json, use_float, split_meshlets):
    cf = ChunkFile.load(path)
    ms = cf.meshset
    n = ms["position"].count
    g = GlbBuilder()
    ARRAY, ELEMENT = 34962, 34963
    F32, I8, U32 = 5126, 5120, 5125

    pos = np.frombuffer(ms["position"].data, dtype="<f4").reshape(-1, 3)
    pmin = pos.min(axis=0).tolist()
    pmax = pos.max(axis=0).tolist()
    attrs = {"POSITION": g.accessor(g.add(ms["position"].data, ARRAY), F32, n, "VEC3", minv=pmin, maxv=pmax)}
    if ms["texcoord0"]:
        attrs["TEXCOORD_0"] = g.accessor(g.add(ms["texcoord0"].data, ARRAY), F32, n, "VEC2")
    if ms["texcoord1"]:
        attrs["TEXCOORD_1"] = g.accessor(g.add(ms["texcoord1"].data, ARRAY), F32, n, "VEC2")

    extensions = []
    if ms["normal"]:
        have_tan = ms["tangent"] is not None and ms["bitangent"] is not None
        signs = tangent_signs(ms, n) if have_tan else None
        if use_float:
            nf = sn8(ms["normal"].data)
            nf /= np.maximum(np.linalg.norm(nf, axis=1, keepdims=True), 1e-8)
            attrs["NORMAL"] = g.accessor(g.add(nf.astype("<f4").tobytes(), ARRAY), F32, n, "VEC3")
            if have_tan:
                tf = np.empty((n, 4), dtype="<f4")
                tf[:, :3] = sn8(ms["tangent"].data)
                tf[:, :3] /= np.maximum(np.linalg.norm(tf[:, :3], axis=1, keepdims=True), 1e-8)
                tf[:, 3] = np.where(signs, 1.0, -1.0)
                attrs["TANGENT"] = g.accessor(g.add(tf.tobytes(), ARRAY), F32, n, "VEC4")
        else:
            extensions.append("KHR_mesh_quantization")
            attrs["NORMAL"] = g.accessor(g.add(ms["normal"].data, ARRAY, stride=4), I8, n, "VEC3", normalized=True)
            if have_tan:
                t = np.frombuffer(ms["tangent"].data, dtype=np.int8).reshape(-1, 4).copy()
                t[:, 3] = np.where(signs, 127, -127)
                attrs["TANGENT"] = g.accessor(g.add(t.tobytes(), ARRAY, stride=4), I8, n, "VEC4", normalized=True)

    gltf = {"asset": {"version": "2.0", "generator": "recon/tools/chk2gltf.py"}, "scene": 0,
            "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": ms["name"] or "mesh"}],
            "materials": [], "meshes": []}
    images, textures = [], []
    mat_index = {}
    for i, m in enumerate(ms["materials"] or [{"name": "default", "texture": None}]):
        mat = {"name": m["name"], "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 1.0}}
        tex = m["texture"] or materials_json.get(m["name"], {}).get("Textures", {}).get("Diffuse")
        if tex:
            uri = os.path.relpath(os.path.join(media_root, *tex.split("/")), os.path.dirname(out_path)).replace("\\", "/")
            images.append({"uri": uri})
            textures.append({"source": len(images) - 1})
            mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": len(textures) - 1}
        gltf["materials"].append(mat)
        mat_index[i] = i

    prims = []
    if ms["type"] == MESHSET_MESHLETS:
        groups = []
        for s in ms["subsets"] or [{"first_meshlet": 0, "meshlet_count": len(ms["meshlets"]), "material": 0}]:
            sel = ms["meshlets"][s["first_meshlet"]:s["first_meshlet"] + s["meshlet_count"]]
            if split_meshlets:
                groups += [([m], s["material"]) for m in sel]
            else:
                groups.append((sel, s["material"]))
        for sel, mat in groups:
            idx = cf.triangle_indices(sel)
            acc = g.accessor(g.add(idx.tobytes(), ELEMENT), U32, len(idx), "SCALAR")
            prims.append({"attributes": attrs, "indices": acc, "material": mat_index.get(mat, 0), "mode": 4})
    else:
        allidx = cf.triangle_indices()
        for s in ms["subsets"] or [{"first_index": 0, "index_count": len(allidx), "material": 0}]:
            idx = allidx[s["first_index"]:s["first_index"] + s["index_count"]]
            acc = g.accessor(g.add(idx.tobytes(), ELEMENT), U32, len(idx), "SCALAR")
            prims.append({"attributes": attrs, "indices": acc, "material": mat_index.get(s["material"], 0), "mode": 4})

    gltf["meshes"].append({"name": ms["name"] or "mesh", "primitives": prims, "extras": {
        "meshletCount": len(ms.get("meshlets", [])), "maxVerts": ms["max_verts"], "maxPrims": ms["max_prims"],
        "bbox": list(ms["bbox"])}})
    if images:
        gltf["images"], gltf["textures"] = images, textures
    if extensions:
        gltf["extensionsUsed"] = gltf["extensionsRequired"] = extensions
    gltf["buffers"] = [{"byteLength": len(g.bin)}]
    gltf["bufferViews"], gltf["accessors"] = g.views, g.accessors

    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    g.bin += b"\0" * (-len(g.bin) % 4)
    with open(out_path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(g.bin)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(g.bin), 0x004E4942) + g.bin)
    return n, len(ms.get("meshlets", []))


def convert_obj(path, out_path):
    cf = ChunkFile.load(path)
    ms = cf.meshset
    pos = np.frombuffer(ms["position"].data, dtype="<f4").reshape(-1, 3)
    tris = cf.triangle_indices().reshape(-1, 3).astype(np.int64) + 1
    with open(out_path, "w") as f:
        f.write(f"# {os.path.basename(path)} converted by recon/tools/chk2gltf.py\no {ms['name']}\n")
        np.savetxt(f, pos, fmt="v %.6g %.6g %.6g")
        full = ms["texcoord0"] is not None and ms["normal"] is not None
        if full:
            uv = np.frombuffer(ms["texcoord0"].data, dtype="<f4").reshape(-1, 2).copy()
            uv[:, 1] = 1.0 - uv[:, 1]
            np.savetxt(f, uv, fmt="vt %.6g %.6g")
            np.savetxt(f, sn8(ms["normal"].data), fmt="vn %.4f %.4f %.4f")
            np.savetxt(f, np.repeat(tris, 3, axis=1), fmt="f %d/%d/%d %d/%d/%d %d/%d/%d")
        else:
            np.savetxt(f, tris, fmt="f %d %d %d")
    return len(pos), len(tris)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("media", help="extracted media root (assets/media)")
    ap.add_argument("out")
    ap.add_argument("--filter", default="")
    ap.add_argument("--float", action="store_true", help="decode normals/tangents to float32")
    ap.add_argument("--split-meshlets", action="store_true", help="one primitive per meshlet")
    ap.add_argument("--obj", action="store_true", help="write OBJ instead of GLB")
    args = ap.parse_args()
    mats = load_materials(args.media)
    for root, _, files in os.walk(args.media):
        for fn in sorted(files):
            if not fn.endswith(".chk"):
                continue
            src = os.path.join(root, fn)
            rel = os.path.relpath(src, args.media)
            if args.filter and args.filter not in rel:
                continue
            dst = os.path.join(args.out, os.path.splitext(rel)[0] + (".obj" if args.obj else ".glb"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if args.obj:
                nv, nt = convert_obj(src, dst)
                print(f"{rel}: {nv} verts, {nt} tris")
            else:
                nv, nm = convert(src, dst, args.media, mats, args.float, args.split_meshlets)
                print(f"{rel}: {nv} verts, {nm} meshlets", flush=True)


if __name__ == "__main__":
    sys.exit(main())
