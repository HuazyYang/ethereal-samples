#ifndef GVDBVOXELIZER_H
#define GVDBVOXELIZER_H
#include <donut/core/object/Foundation.h>
#include <gvdb/GPDevice.h>
#include <donut/core/math/math.h>

namespace donut::vfs {
class IFileSystem;
}

namespace gvdb {
class GVDB;
}

namespace SampleUtils {

DONUT_IID(IVoxelizer, "60dcd346-537e-4f2b-ac21-84fd2fd2e235")
struct IVoxelizer : donut::IObject {
    DONUT_DECLARE_UUID_TRAITS(IVoxelizer)

    virtual donut::FRESULT solidVoxelize(gvdb::GVDB *pGVDB, int channel,
                                    donut::gp::IBuffer *pVertBuffer,
                                    donut::gp::IBuffer *pIndexBuffer,
                                    int numIndices, const dm::box3 &modelBounds,
                                    const dm::affine3 &matModelToVolume, float valSurface,
                                    float valInside) = 0;
};

donut::FRESULT createVoxelizer(donut::gp::IDevice *device,
                                  donut::vfs::IFileSystem *vfs, IVoxelizer **ppVoxelizer);

};  // namespace gvdb

#endif /* GVDBVOXELIZER_H */
