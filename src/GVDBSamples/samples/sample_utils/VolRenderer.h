#ifndef VOLRENDERER_H
#define VOLRENDERER_H
#include <nvrhi/core/foundation.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/math/math.h>
#include <nvrhi/core/autoptr.h>
#include <gvdb/GVDB.h>

namespace SampleUtils {

enum class VolShadeType {
    VOXEL = 0,
    SECTION2D = 1,
    SECTION3D = 2,
    EMPTYSKIP = 3,
    TRILINEAR = 4,
    TRICUBIC = 5,
    LEVELSET = 6,
    VOLUME = 7
};

class Camera;

// TODO(migration): dropped - base `ethereal::UserAllocated` exists in the old
// ethereal fork; nvrhi::UserAllocated has protected operator new/delete.
class VolRenderer {
 public:
    VolRenderer(donut::gp::IDevice *device, donut::vfs::IFileSystem *vfs);
    ~VolRenderer();

    void setLinearTransferFunc(float t0, float t1, dm::float4 v0, dm::float4 v1);
    void commitTransferFunc();

    // Sets how far each iteration marches in voxel space when ray marching.
    void setSteps(float directStep, float shadowStep, float fineStep);

    void setVolumeRange(float viso, float vmin, float vmax);

    void setExtinct(float a, float b, float c);

    void setCutoff(float a, float b, float c);

    void setBackgroundColor(dm::float4 color);

    void setCrossSection(dm::float3 pos, dm::float3 norm);

    void setShading(VolShadeType s);

    void setShadowParams(float x, float y, float z);

    void setSample(int s);

    void setFrame(int f);

    void setFilterMode(int f);

    void setRayNormalBias(float bias);

    void setUsedGVDBChannel(int chann);

    void setEpsilon(float eps);

    // GVDB's world space is now equal to its index space;
    // use SetTransform to apply your own arbitrary affine transformations void
    void setTransform(dm::float3 preTrans, dm::float3 scale, dm::float3 angles,
                      dm::float3 trans);

    void setDepthBuffer(donut::gp::IBuffer *depthBuffer);

    void render(gvdb::GVDB *pVDB, int channel, donut::gp::IBuffer *pRenderBuffer,
                Camera *pCamera, int w, int h, Camera *pLight);

    const dm::float4x4 &getTransform();

 private:
    void prepareRender(Camera *pCamera, int w, int h, Camera *pLight);

    donut::gp::IDevice *m_device;
    nvrhi::AutoPtr<donut::gp::IDeviceQueue> m_queue;
    nvrhi::AutoPtr<donut::gp::IKernel> m_rayDeepKernel;
    nvrhi::AutoPtr<donut::gp::IKernel> m_voxelKernel;
    nvrhi::AutoPtr<donut::gp::IKernel> m_trilinearKernel;
    nvrhi::AutoPtr<donut::gp::IKernel> m_tricubicKernel;
    nvrhi::AutoPtr<donut::gp::IKernel> m_surfaceDeepKernel;
    nvrhi::AutoPtr<donut::gp::IKernel> m_levelsetKernel;
    nvrhi::AutoPtr<donut::gp::IKernel> m_emptySkipKernel;
    nvrhi::AutoPtr<donut::gp::IKernel> m_section2DKernel;
    nvrhi::AutoPtr<donut::gp::IKernel> m_section3DKernel;

    nvrhi::AutoPtr<donut::gp::IBuffer> m_scnInfoBuffer;
    nvrhi::AutoPtr<donut::gp::IBuffer> m_transferFuncGPU;
    nvrhi::AutoPtr<donut::gp::IBuffer> m_depthBuffer;

    // Shadow parameters(independent of method used)
    dm::float3 m_shadowParams;

    // Volume import settings
    dm::float3 m_VclipMin, m_VclipMax;
    dm::float3 m_Vleaf;
    dm::float3 m_Vframes;

    // Transform function
    dm::float3 m_transferVec;  // x = alpha, y = gain
    std::vector<dm::float4> m_transferFunc;

    // Volume settings
    dm::float3 m_Vthreshold;
    dm::float3 m_extinct;
    dm::float3 m_steps;  // Direct step, shadow step, fine step
    dm::float3 m_cutoff;
    dm::float4 m_backgroundColor;

    // Cross sections
    dm::float3 m_sectionPoint;
    dm::float3 m_sectionNormal;

    // Rendering settings
    VolShadeType m_shading;
    int m_filterMode;
    int m_frame;
    int m_sample;

    float m_rayNormalBias;
    int m_usedGVDBChannel;
    float m_epsilon;

    dm::float3 m_Pretrans, m_Angles, m_Trans, m_Scale;
    dm::float4x4 m_Xform, m_InvXform, m_InvRot;
};

};  // namespace SampleUtils

#endif /* VOLRENDERER_H */
