// OBJ -> Donut MeshInfo (one geometry, like the glTF importer builds them) and
// GPU copies for the voxelizer / raster passes.
#include "ObjMesh.h"
#include "SampleTypes.h"
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>

namespace SampleUtils {

using namespace donut;

nvrhi::AutoPtr<engine::MeshInfo> loadObjMesh(vfs::IFileSystem *vfs, const std::filesystem::path &path, float scale,
                                             dm::float3 offset) {
    nvrhi::AutoPtr<vfs::IFileSystem> nativeFS;
    if (!vfs) {
        nativeFS = MAKE_RC_OBJ_PTR(vfs::NativeFileSystem);
        vfs = nativeFS;
    }
    std::string name = path.generic_string();
    TriangleMesh tri = ObjLoader()(vfs, name.c_str());
    if (tri.positions.empty() || tri.indices.empty()) {
        log::error("loadObjMesh: cannot load %s", name.c_str());
        return nullptr;
    }

    const size_t numVertices = tri.positions.size();
    const size_t numIndices = tri.indices.size() * 3;

    auto buffers = MAKE_RC_OBJ_PTR(engine::BufferGroup);
    buffers->positionData.resize(numVertices);
    buffers->normalData.resize(numVertices);
    buffers->indexData.resize(numIndices);

    dm::box3 bounds = dm::box3::empty();
    for (size_t i = 0; i < numVertices; ++i) {
        dm::float3 p = tri.positions[i] * scale + offset;
        buffers->positionData[i] = p;
        bounds |= p;
    }
    if (tri.normals.size() == numVertices) {
        for (size_t i = 0; i < numVertices; ++i) buffers->normalData[i] = dm::vectorToSnorm8(tri.normals[i]);
    } else {
        // Area-weighted vertex normals from the faces.
        std::vector<dm::float3> normals(numVertices, dm::float3::zero());
        for (const dm::int3 &t : tri.indices) {
            dm::float3 n = dm::cross(tri.positions[t.y] - tri.positions[t.x], tri.positions[t.z] - tri.positions[t.x]);
            normals[t.x] += n;
            normals[t.y] += n;
            normals[t.z] += n;
        }
        for (size_t i = 0; i < numVertices; ++i) {
            dm::float3 n = normals[i];
            n = dm::lengthSquared(n) > 0.f ? dm::normalize(n) : dm::float3(0.f, 1.f, 0.f);
            buffers->normalData[i] = dm::vectorToSnorm8(n);
        }
    }
    for (size_t i = 0; i < tri.indices.size(); ++i) {
        buffers->indexData[i * 3 + 0] = uint32_t(tri.indices[i].x);
        buffers->indexData[i * 3 + 1] = uint32_t(tri.indices[i].y);
        buffers->indexData[i * 3 + 2] = uint32_t(tri.indices[i].z);
    }

    auto geometry = MAKE_RC_OBJ_PTR(engine::MeshGeometry);
    geometry->material = MAKE_RC_OBJ_PTR(engine::Material);
    geometry->material->name = "obj default";
    geometry->objectSpaceBounds = bounds;
    geometry->indexOffsetInMesh = 0;
    geometry->vertexOffsetInMesh = 0;
    geometry->numIndices = uint32_t(numIndices);
    geometry->numVertices = uint32_t(numVertices);
    geometry->type = engine::MeshGeometryPrimitiveType::Triangles;

    auto mesh = MAKE_RC_OBJ_PTR(engine::MeshInfo);
    mesh->name = path.filename().string();
    mesh->type = engine::MeshType::Triangles;
    mesh->buffers = buffers;
    mesh->geometries.push_back(geometry);
    mesh->objectSpaceBounds = bounds;
    mesh->indexOffset = 0;
    mesh->vertexOffset = 0;
    mesh->totalIndices = uint32_t(numIndices);
    mesh->totalVertices = uint32_t(numVertices);
    return mesh;
}

namespace {
std::vector<dm::float3> decodeNormals(const engine::BufferGroup *buffers, size_t count) {
    std::vector<dm::float3> normals(count, dm::float3(0.f, 1.f, 0.f));
    for (size_t i = 0; i < count && i < buffers->normalData.size(); ++i)
        normals[i] = dm::snorm8ToVector<3>(buffers->normalData[i]);
    return normals;
}
}  // namespace

MeshGPBuffers uploadMeshGP(gp::IDevice *device, gp::IDeviceQueue *queue, const engine::MeshInfo *mesh) {
    MeshGPBuffers out;
    if (!device || !queue || !mesh || !mesh->buffers) return out;
    const engine::BufferGroup *buffers = mesh->buffers;
    const size_t numVertices = mesh->totalVertices;
    const size_t numIndices = mesh->totalIndices;
    if (numVertices == 0 || numIndices == 0) return out;

    const dm::float3 *positions = buffers->positionData.data() + mesh->vertexOffset;
    const uint32_t *indices = buffers->indexData.data() + mesh->indexOffset;
    std::vector<dm::float3> normals = decodeNormals(buffers, numVertices);

    gp::BufferDesc desc;
    desc.byteSize = numVertices * sizeof(dm::float3);
    UT_V_GP(device->createBuffer(desc, &out.positions));
    UT_V_GP(queue->writeBuffer(out.positions, positions, desc.byteSize, 0));
    UT_V_GP(device->createBuffer(desc, &out.normals));
    UT_V_GP(queue->writeBuffer(out.normals, normals.data(), desc.byteSize, 0));
    desc.byteSize = numIndices * sizeof(uint32_t);
    UT_V_GP(device->createBuffer(desc, &out.indices));
    UT_V_GP(queue->writeBuffer(out.indices, indices, desc.byteSize, 0));
    out.numVertices = uint32_t(numVertices);
    out.numIndices = uint32_t(numIndices);
    return out;
}

MeshNVRHIBuffers uploadMeshNVRHI(nvrhi::IDevice *device, nvrhi::ICommandList *commandList, const engine::MeshInfo *mesh) {
    MeshNVRHIBuffers out;
    if (!device || !commandList || !mesh || !mesh->buffers) return out;
    const engine::BufferGroup *buffers = mesh->buffers;
    const size_t numVertices = mesh->totalVertices;
    const size_t numIndices = mesh->totalIndices;
    if (numVertices == 0 || numIndices == 0) return out;

    const dm::float3 *positions = buffers->positionData.data() + mesh->vertexOffset;
    const uint32_t *indices = buffers->indexData.data() + mesh->indexOffset;
    std::vector<dm::float3> normals = decodeNormals(buffers, numVertices);

    auto upload = [&](nvrhi::BufferHandle &handle, const void *data, size_t bytes, bool index, const char *name) {
        nvrhi::BufferDesc desc;
        desc.byteSize = bytes;
        desc.isVertexBuffer = !index;
        desc.isIndexBuffer = index;
        desc.debugName = name;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        device->createBuffer(desc, &handle);
        commandList->beginTrackingBufferState(handle, nvrhi::ResourceStates::CopyDest);
        commandList->writeBuffer(handle, data, bytes);
        commandList->setPermanentBufferState(handle,
                                             index ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::VertexBuffer);
    };
    upload(out.positions, positions, numVertices * sizeof(dm::float3), false, "mesh positions");
    upload(out.normals, normals.data(), numVertices * sizeof(dm::float3), false, "mesh normals");
    upload(out.indices, indices, numIndices * sizeof(uint32_t), true, "mesh indices");
    commandList->commitBarriers();
    out.numVertices = uint32_t(numVertices);
    out.numIndices = uint32_t(numIndices);
    return out;
}

}  // namespace SampleUtils
