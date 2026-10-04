# Asteroids demo – data formats

All addresses refer to `Asteroids.exe` (image base 0x140000000).

## media.db

SQLite 3 database, single table:

```sql
CREATE TABLE files (name TEXT PRIMARY KEY ON CONFLICT REPLACE,
                    mtime INTEGER, compressed INTEGER, original_size INTEGER, data BLOB)
```

Reader: `SQLiteFileSystem` (ctor `0x14009A720`, `readFile` `0x14009AA70`, constructed in `0x1400230B0`
with `encrypted = true`). Lookup query: `SELECT data, compressed, original_size FROM files WHERE name=?1 LIMIT 1`.
Paths are normalized to forward slashes and converted to narrow strings before lookup.

Per-entry decoding:

| step | detail |
|---|---|
| key | `BCryptKeyDerivation(PBKDF2, SHA1, password, salt = file name (UTF-8), iterations = 1000)` → 16 bytes |
| password | `HjLxk8CwekjjquQM` (wide string assembled on the stack in `0x1400230B0`, then converted to ANSI) |
| cipher | AES-128-CBC, IV = `data[0:16]`, ciphertext = `data[16:]`, no padding removal (size is padded to 16) |
| compression | if `compressed > 0`: LZ4 block (`LZ4_decompress_safe`, `0x140147DB0`) of `payload[:compressed]` into `original_size` bytes; otherwise `payload[:original_size]` |

Tooling: `tools/mediadb.py` (reader), `tools/extract_media.py` (bulk extraction, restores mtime).

## NVSP shader permutation blobs (`shaders/**.bin`)

Loader: `findPermutationInBlob` `0x1401ADB10`, `enumeratePermutationsInBlob` `0x1401AD8C0`,
error text `0x1401ADD40`. A file not starting with `NVSP` is a single shader container (the
`<default>` permutation, used only when no defines are requested).

Field names from `nvrhi/common/shader-blob.h` (`struct ShaderBlobEntry`) in the waveworks2 nvrhi SDK:
```
char magic[4] = "NVSP"
repeat:
  u32 hashKeySize
  u32 dataSize
  u32 dataCrc        // CRC32 of data
  u32 defineHash     // CRC32 of key; the runtime hashes "NAME=VALUE;" for each requested define
  u32 flags
  char key[keyLength] // e.g. "IS_SHIP=0;_ASTEROIDS=1;"
  u8   data[dataLength] // DXIL container
```

Every container is DXC output (`SFI0 ISG1 OSG1 PSV0 ILDN DXIL`), shader model 6.0. Full resource reflection
(cbuffer struct and field names) is preserved, but no source or debug info. The `*MS` / `*TS` / `particles_ms`
"mesh shaders" are vertex-stage DXIL that use NVAPI HLSL extensions (`g_NvidiaExt`, `u7`). This is the 2018
Turing NVAPI mesh-shader path, consumed through `NvAPI_D3D12_CreateGraphicsPipelineState`, not DX12 Ultimate mesh shaders.

Tooling: `tools/split_nvsp.py` → `assets/shaders_split/<shader>/<permutation>.dxil` plus `dxc -dumpbin` listing.

## NVDACHNK chunk files (`*.chk`)

Loader: `ChunkFile::deserialize` `0x1400B4560`, string table `0x1400B39A0`, MeshSet `0x1400B3080`,
data stream `0x1400B37A0`, subsets `0x1400B3B50`, instances `0x1400B2ED0`, materials `0x1400B2CE0`.
Consumer (meshlet asset loading): `0x1400111C0` / `0x140055730`.

### Container

```
struct FileHeader {            // 20 bytes
  char signature[8];           // "NVDACHNK"
  u32  version;                // 0x100
  u32  numChunks;              // 1 .. 1000000
  u32  chunkTableOffset;
};
struct ChunkTableEntry {       // 32 bytes
  u32 id;                      // referenced by other chunks; 0xFFFFFFFF = none
  u32 type;
  u32 version;                 // 0x100 for every type
  u32 unused;                  // not read by the loader (uninitialized writer data)
  u64 offset;                  // from file start
  u64 size;
};
```

This is donut's `donut::chunk` format (`D:\ps\repo\ethereal\donut`, branch `main`: `src/core/chunk/chunkDescs.h`,
`include/donut/core/chunk/chunk.h`) in an earlier (2018) revision. The names below follow donut. Where the 2018
layout differs from donut's 2021 one, it is noted.

| type | donut name | meaning |
|---|---|---|
| 0x100 | CHUNKTYPE_STREAM | data stream |
| 0x110 | CHUNKTYPE_STRINGS_TABLE | string table (at most one) |
| 0x200 | CHUNKTYPE_MESHSET | MeshSet (the loader picks the first one) |
| 0x201 | CHUNKTYPE_MESH_INFOS | mesh / meshlet infos (subsets) |
| 0x202 | CHUNKTYPE_MESH_INSTANCES | instances |
| 0x203 | CHUNKTYPE_MESH_NODES | node hierarchy (not present in shipped files) |
| 0x400 | CHUNKTYPE_MATERIALS | materials (2018 only, referenced from the MeshSet; dropped in donut 2021) |

### 0x110 string table
```
u32 flags; u32 nstrings;
struct { u64 offset; u64 length; } entries[count];   // offset relative to the end of this array, length includes NUL
char data[];
```
Other chunks refer to strings by u64 index (`~0ull` = null).

### 0x100 data stream
```
u64 flags;     // bits 0..3 Type (1=UINT8, 2=UINT16, 3=UINT32, 4=FP16, 5=FP32, 6=STRING),
               // bits 4..5 Vary (1=VARY_NONE, 2=VERTEX, 3=FACE),
               // bits 6..9 Semantic (1=POSITION, 2=NORMAL, 3=TANGENT, 4=BITANGENT, 5=TEXCOORD, 6=COLOR, 7=INDEX, 8=MESHLET_INFO)
u64 elemCount;
u64 elemSize;
u8  data[count*stride];
```
| semantic | type | stride | encoding |
|---|---|---|---|
| POSITION | f32 | 12 | float3 |
| TEXCOORD | f32 | 8 | float2 |
| NORMAL / TANGENT / BITANGENT | u32 | 4 | snorm8 x,y,z; w byte = 0 (decoded in shader by hand: `x>>7` sign) |
| INDEX (u32) | u32 | 4 | meshlet vertex-index list (global vertex ids) or triangle indices for type-0 sets |
| INDEX (u8) | u8 | 1 | meshlet-local primitive indices, 3 per triangle |
| MESHLET | u32 | 16 | see below |

### 0x200 MeshSet (128 bytes)
```
+0   u32 flags            // & 0xF: 0 = triangle mesh, 1 = meshlets
+4   u32 maxVerts         // 64 in all shipped files
+8   u32 maxPrims         // 100 in all shipped files ("wrong maxVerts/maxPrims" is checked by the demo)
+12  u32 unknown
+16  u64 name             // string index
+24  u32 positionStream
+28  u32 texcoord0Stream
+32  u32 texcoord1Stream
+36  u32 normalStream
+40  u32 tangentStream
+44  u32 bitangentStream
+48  u32 indexStream      // meshlets: u32 vertex-index list; triangles: u32 index buffer
+52  u32 primStream       // meshlets only: u8 local primitive indices
+56  u32 meshletStream    // meshlets only
+60  u32 unused[7]        // streamChunkIds[16] spans +24..+88
+88  u32 minfosChunkId
+92  u32 instancesChunkId
+96  u32 nodesChunkId
+100 u32 materialsChunkId // 2018 only
+104 float bboxMin[3], bboxMax[3]
```

### Meshlet descriptor (16 bytes)
```
u32 w0;            // bits 24..31 vertexCount, bits 0..23 three u8 culling values (quantized, decoded in asteroidMS)
u32 w1;            // bits 24..31 primCount,   bits 0..23 three u8 culling values
u32 vertexOffset;  // into the u32 vertex-index stream
u32 primOffset;    // byte offset into the u8 primitive stream
```
Triangle `t` of meshlet `m`: `vtx[k] = vertexIndices[m.vertexOffset + prims[m.primOffset + 3t + k]]`.
The resulting winding is CCW with respect to the stored normals.

### 0x201 mesh infos
```
u32 flags;  // & 0xF: MESH(0) / MESHLET(1), must match the MeshSet type
u32 nelems;
struct MeshInfoBase { u64 name; u32 materialId; box3 bbox; u32 padding; };   // 2018: no materialName (donut 2021 adds it)
struct MeshletInfo : MeshInfoBase { u32 firstMeshlet, numMeshlets; };          // 48 bytes
struct MeshInfo    : MeshInfoBase { u32 firstVertex, numVertices, firstIndex, numIndices; };  // 56 bytes
```
The loader replaces `name` with a pointer (string index → `const char*`) in place. The padding field holds
uninitialized writer data.

### 0x202 instances
```
u32 ninstances;
struct MeshInstance { u64 name; u32 minfoId; u32 nodeId; affine3 transform; box3 bbox; float3 center; u32 padding; }; // 104 bytes
```

### 0x203 nodes
```
u32 nnodes; u32 rootId;
struct MeshNode { u64 name; u32 parentId, siblingId, instanceId; affine3 transform; box3 bbox; float3 center; }; // 104 bytes (2018: no ctm)
```

### 0x400 materials
```
u32 count;
struct { u64 name; u64 diffuseTexture; } entries[count];   // packed, 4-byte aligned
```
Shipped files use the name only (for example `Tiny003_Material`). Textures come from `Clustered/materials.json`.

Tooling: `tools/chk.py` (parser), `tools/chk2gltf.py` (GLB with KHR_mesh_quantization by default, or `--float`, `--obj`, `--split-meshlets`).

## Stars.buf

Raw `StructuredBuffer<StarInstance>` with no header (from the `real_stars_vs` reflection): 16-byte records
`{ float Ra; float Dec; float Mag; int CatNumber; }`. There are 258,944 records (= 4,046 instances of 64 stars).
Ra and Dec are in degrees and Mag is the visual magnitude (-1.6 … 11.9). The file is sorted by magnitude.
CatNumber is the SAO catalogue number: record 0 is Sirius (SAO 151881), then Canopus and Vega. Uploaded as the "Star instance buffer" (0x140066BF0).
