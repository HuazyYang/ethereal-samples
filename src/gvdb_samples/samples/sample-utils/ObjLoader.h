#ifndef OBJLOADER_H
#define OBJLOADER_H
#include <vector>
#include <donut/core/math/math.h>
#include <donut/core/vfs/VFS.h>

namespace SampleUtils {

struct TriangleMesh {
    std::vector<dm::int3> indices;
    std::vector<dm::float3> positions;
    std::vector<dm::float3> normals;
    dm::box3 bounds;
};

struct ObjLoader {
    TriangleMesh operator()(donut::vfs::IFileSystem *pFS, const char *filename) const;
};

}  // namespace SampleUtils

#endif /* OBJLOADER_H */
