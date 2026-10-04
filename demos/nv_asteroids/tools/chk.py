"""Parser for NVDACHNK (.chk) mesh files used by the Asteroids meshlet demo.

Reversed from Asteroids.exe ChunkFile/MeshSet loader (0x1400B4560 header, 0x1400B3080 MeshSet,
0x1400B37A0 data stream, 0x1400B3B50 subsets, 0x1400B2ED0 instances, 0x1400B2CE0 materials,
0x1400B39A0 string table). See docs/formats.md for the full description.
"""
import struct

import numpy as np

SIGNATURE = b"NVDACHNK"

# chunk types
# Names follow donut::chunk (donut core/chunk/chunkDescs.h); the 2018 binary predates some fields.
CHUNK_STREAM = 0x100
CHUNK_STRINGS = 0x110
CHUNK_MESHSET = 0x200
CHUNK_MESH_INFOS = 0x201
CHUNK_MESH_INSTANCES = 0x202
CHUNK_MESH_NODES = 0x203
CHUNK_MATERIALS = 0x400

# data stream element type (bits 0..3)
TYPE_U8, TYPE_U16, TYPE_U32, TYPE_FP16, TYPE_FP32, TYPE_STRING = 1, 2, 3, 4, 5, 6
# data stream vary (bits 4..5)
VARY_NONE, VARY_VERTEX, VARY_FACE, VARY_FACE_VERTEX = 1, 2, 3, 4
# data stream semantics (bits 6..9)
SEM_POSITION, SEM_NORMAL, SEM_TANGENT, SEM_BITANGENT, SEM_TEXCOORD, SEM_COLOR, SEM_INDEX, SEM_MESHLET_INFO = range(1, 9)

MESHSET_TRIANGLES = 0
MESHSET_MESHLETS = 1


class Stream:
    def __init__(self, raw):
        h, self.count, self.stride = struct.unpack_from("<QQQ", raw, 0)
        self.elem_type = h & 0xF
        self.vary = (h >> 4) & 0x3
        self.semantic = (h >> 6) & 0xF
        self.data = raw[24:24 + self.count * self.stride]


class Meshlet:
    __slots__ = ("vertex_count", "prim_count", "vertex_offset", "prim_offset", "packed0", "packed1")

    def __init__(self, w0, w1, vo, po):
        self.vertex_count = w0 >> 24
        self.prim_count = w1 >> 24
        self.vertex_offset = vo
        self.prim_offset = po          # byte offset into the u8 primitive stream (3 bytes per triangle)
        self.packed0 = w0 & 0xFFFFFF   # quantized culling data (3 x u8), decoded in asteroidMS/TS
        self.packed1 = w1 & 0xFFFFFF


class ChunkFile:
    def __init__(self, data):
        if data[:8] != SIGNATURE:
            raise ValueError("invalid signature")
        self.version, count, table = struct.unpack_from("<III", data, 8)
        self.chunks = {}
        for i in range(count):
            cid, ctype, cver, _, off, size = struct.unpack_from("<IIIIQQ", data, table + 32 * i)
            self.chunks[cid] = (ctype, cver, data[off:off + size])
        self.strings = self._load_strings()
        self.meshset = self._load_meshset()

    @classmethod
    def load(cls, path):
        with open(path, "rb") as f:
            return cls(f.read())

    def _of_type(self, ctype):
        return [(cid, raw) for cid, (t, _, raw) in self.chunks.items() if t == ctype]

    def _load_strings(self):
        tables = self._of_type(CHUNK_STRINGS)
        if not tables:
            return []
        raw = tables[0][1]
        count = struct.unpack_from("<I", raw, 4)[0]
        base = 8 + 16 * count
        out = []
        for i in range(count):
            off, length = struct.unpack_from("<QQ", raw, 8 + 16 * i)
            out.append(raw[base + off:base + off + length].split(b"\0", 1)[0].decode())
        return out

    def string(self, idx):
        return None if idx in (-1, 0xFFFFFFFFFFFFFFFF) or idx >= len(self.strings) else self.strings[idx]

    def stream(self, cid):
        if cid == 0xFFFFFFFF:
            return None
        ctype, _, raw = self.chunks[cid]
        assert ctype == CHUNK_STREAM
        return Stream(raw)

    def _load_meshset(self):
        (cid, raw), = self._of_type(CHUNK_MESHSET)
        f = struct.unpack_from("<III", raw, 0)
        ms = {"type": f[0] & 0xF, "max_verts": f[1], "max_prims": f[2]}
        ms["name"] = self.string(struct.unpack_from("<Q", raw, 16)[0])
        ids = struct.unpack_from("<20I", raw, 24)
        ms["position"] = self.stream(ids[0])
        ms["texcoord0"] = self.stream(ids[1])
        ms["texcoord1"] = self.stream(ids[2])
        ms["normal"] = self.stream(ids[3])
        ms["tangent"] = self.stream(ids[4])
        ms["bitangent"] = self.stream(ids[5])
        if ms["type"] == MESHSET_MESHLETS:
            ms["vertex_indices"] = self.stream(ids[6])
            ms["prim_indices"] = self.stream(ids[7])
            md = self.stream(ids[8])
            ms["meshlets"] = [Meshlet(*struct.unpack_from("<4I", md.data, 16 * i)) for i in range(md.count)]
        else:
            ms["indices"] = self.stream(ids[6])
        ms["subsets_chunk"], ms["instances_chunk"], ms["nodes_chunk"], ms["materials_chunk"] = ids[16:20]
        ms["bbox"] = struct.unpack_from("<6f", raw, 104)
        ms["subsets"] = self._load_subsets(ms)
        ms["instances"] = self._load_instances(ms["instances_chunk"])
        ms["materials"] = self._load_materials(ms["materials_chunk"])
        return ms

    def _load_subsets(self, ms):
        cid = ms["subsets_chunk"]
        if cid == 0xFFFFFFFF:
            return []
        raw = self.chunks[cid][2]
        stype, count = struct.unpack_from("<II", raw, 0)
        out = []
        # MeshInfoBase (2018): u64 name; u32 materialId; box3 bbox; u32 padding
        for i in range(count):
            if stype == MESHSET_MESHLETS:   # MeshletInfo: + u32 firstMeshlet, numMeshlets (48 bytes)
                p = 8 + 48 * i
                name, material = struct.unpack_from("<QI", raw, p)
                bbox = struct.unpack_from("<6f", raw, p + 12)
                first, n = struct.unpack_from("<II", raw, p + 40)
                out.append({"name": self.string(name), "first_meshlet": first, "meshlet_count": n,
                            "material": material, "bbox": bbox})
            else:                           # MeshInfo: + u32 firstVertex, numVertices, firstIndex, numIndices (56 bytes)
                p = 8 + 56 * i
                name, material = struct.unpack_from("<QI", raw, p)
                bbox = struct.unpack_from("<6f", raw, p + 12)
                fv, nv, fi, ni = struct.unpack_from("<4I", raw, p + 40)
                out.append({"name": self.string(name), "first_vertex": fv, "vertex_count": nv,
                            "first_index": fi, "index_count": ni, "material": material, "bbox": bbox})
        return out

    def _load_instances(self, cid):
        """MeshInstance: u64 name; u32 minfoId, nodeId; affine3; box3; float3 center; u32 pad (104 bytes)."""
        if cid == 0xFFFFFFFF:
            return []
        raw = self.chunks[cid][2]
        count = struct.unpack_from("<I", raw, 0)[0]
        out = []
        for i in range(count):
            p = 4 + 104 * i
            name, minfo, node = struct.unpack_from("<QIi", raw, p)
            m = struct.unpack_from("<12f", raw, p + 16)
            bbox = struct.unpack_from("<6f", raw, p + 64)
            center = struct.unpack_from("<3f", raw, p + 88)
            out.append({"name": self.string(name), "minfo": minfo, "node": node, "affine3": m,
                        "bbox": bbox, "center": center})
        return out

    def _load_materials(self, cid):
        if cid == 0xFFFFFFFF:
            return []
        raw = self.chunks[cid][2]
        count = struct.unpack_from("<I", raw, 0)[0]
        out = []
        for i in range(count):
            name, tex = struct.unpack_from("<QQ", raw, 4 + 16 * i)
            out.append({"name": self.string(name), "texture": self.string(tex)})
        return out

    def triangle_indices(self, meshlets=None):
        """Expand meshlets into a flat, CCW triangle-list of global vertex indices (uint32 ndarray)."""
        ms = self.meshset
        if ms["type"] != MESHSET_MESHLETS:
            return np.frombuffer(ms["indices"].data, dtype="<u4").copy()
        vi = np.frombuffer(ms["vertex_indices"].data, dtype="<u4")
        prims = np.frombuffer(ms["prim_indices"].data, dtype=np.uint8)
        sel = meshlets if meshlets is not None else ms["meshlets"]
        vo = np.array([m.vertex_offset for m in sel], dtype=np.int64)
        po = np.array([m.prim_offset for m in sel], dtype=np.int64)
        n = np.array([3 * m.prim_count for m in sel], dtype=np.int64)
        starts = np.cumsum(n) - n
        local = np.arange(n.sum(), dtype=np.int64) - np.repeat(starts, n)
        return vi[np.repeat(vo, n) + prims[np.repeat(po, n) + local]].astype(np.uint32)
