#ifndef SAMPLE_UTILS_OBJMESH_H
#define SAMPLE_UTILS_OBJMESH_H
// OBJ models as Donut scene-graph meshes: a MeshInfo with CPU buffers
// (positions, normals, indices) plus the GPU copies the samples need: buffers
// of the CUDA device for the voxelizer / OptiX and of the graphics device for
// raster passes.
#include <nvrhi/nvrhi.h>
#include <nvrhi/core/autoptr.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/SceneGraph.h>
#include <donut/engine/SceneTypes.h>
#include <gvdb/GVDBCompute.h>
#include <sample-utils/ObjLoader.h>

namespace SampleUtils {

// Loads an OBJ into a MeshInfo (one geometry, positions + normals + uint32
// indices, object space bounds). scale / offset are baked into the vertices
// like the reference Scene::AddModel(file, scale, tx, ty, tz).
nvrhi::AutoPtr<donut::engine::MeshInfo> loadObjMesh(donut::vfs::IFileSystem *vfs, const std::filesystem::path &path,
                                                    float scale = 1.f, dm::float3 offset = dm::float3::zero());

// Mesh data uploaded to buffers of the CUDA device (tightly packed float3 positions, float3 normals, uint3
// indices); the uploads are recorded into the compute context.
struct MeshCudaBuffers {
    nvrhi::BufferHandle positions;
    nvrhi::BufferHandle normals;
    nvrhi::BufferHandle indices;
    uint32_t numVertices = 0;
    uint32_t numIndices = 0;   // 3 per triangle
};
MeshCudaBuffers uploadMeshCuda(gvdb::IComputeContext *context, const donut::engine::MeshInfo *mesh);

// Mesh data uploaded to nvrhi vertex / index buffers for raster passes.
struct MeshNVRHIBuffers {
    nvrhi::BufferHandle positions;
    nvrhi::BufferHandle normals;
    nvrhi::BufferHandle indices;
    uint32_t numVertices = 0;
    uint32_t numIndices = 0;
};
MeshNVRHIBuffers uploadMeshNVRHI(nvrhi::IDevice *device, nvrhi::ICommandList *commandList,
                                 const donut::engine::MeshInfo *mesh);

}  // namespace SampleUtils

#endif /* SAMPLE_UTILS_OBJMESH_H */
