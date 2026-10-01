#include "SampleTypes.h"
#include "VolRenderer.h"
#include "Camera.h"
// NOTE(migration): countof() came from the old ethereal fork's <ethereal/core/math/basics.h>;
// donut has no equivalent, so std::size() is used instead.
#include <iterator>

namespace SampleUtils {

using namespace donut;

struct GVDB_ALIGN(16) ScnInfo {
    int			width;
    int			height;
    float		camnear;
    float   	camfar;
    dm::float3	campos;
    dm::float3	cams;
    dm::float3	camu;
    dm::float3	camv;
    dm::float3	light_pos;
    dm::float3	slice_pnt;
    dm::float3	slice_norm;
    dm::float3	shadow_params;
    dm::float4	backclr;
    float		xform[16];
    float		invxform[16];
    float		invxrot[16];
    float		bias;		
    char		shading;
    char		filtering;		
    int			frame;
    int			samples;				
    dm::float3	extinct;
    dm::float3	steps;
    dm::float3	cutoff;
    dm::float3	thresh;
    float       epsilon;
    int         gvdb_channel;
    donut::gp::NativeHandle    transfer;
    donut::gp::NativeHandle    outbuf;
    donut::gp::NativeHandle    dbuf;		
};

VolRenderer::VolRenderer(donut::gp::IDevice* device, donut::vfs::IFileSystem *vfs): m_device(device) {

    donut::gp::DeviceQueueDesc queueDesc;
    queueDesc.priority = gp::DeviceQueuePriority::Normal;
    UT_V_GP(m_device->createDeviceQueue(queueDesc, &m_queue));

    // Load device module
    {
        donut::AutoPtr<donut::IDataBlob> pBlob;
        UT_V_GP(vfs->readFile("sample_utils/kernels/cuda_gvdb_raycast.ptx", &pBlob));
        size_t len = pBlob->GetSize();
        pBlob->Resize(len + 1);
        ((char *)pBlob->GetDataPtr())[len] = 0;

        AutoPtr<gp::IModule> pModule;
        UT_V_GP(m_device->createModule({}, pBlob->GetDataPtr(), len + 1, &pModule));

        UT_V_GP(pModule->getKernel("gvdbRaySurfaceVoxel", &m_voxelKernel));
        UT_V_GP(pModule->getKernel("gvdbRaySurfaceTrilinear", &m_trilinearKernel));
        UT_V_GP(pModule->getKernel("gvdbSurfaceTrilinear", &m_trilinearKernel));
        UT_V_GP(pModule->getKernel("gvdbSurfaceTricubic", &m_tricubicKernel));
        UT_V_GP(pModule->getKernel("gvdbSurfaceDeep", &m_rayDeepKernel));
        UT_V_GP(pModule->getKernel("gvdbRayLevelSet", &m_levelsetKernel));
        UT_V_GP(pModule->getKernel("gvdbRayEmptySkip", &m_emptySkipKernel));
        UT_V_GP(pModule->getKernel("gvdbSection2D", &m_section2DKernel));
        UT_V_GP(pModule->getKernel("gvdbSection3D", &m_section3DKernel));
    }

    m_device = device;

    donut::gp::BufferDesc bufDesc;
    bufDesc.isStaging = true;
    bufDesc.byteSize = sizeof(ScnInfo);
    m_device->createBuffer(bufDesc, &m_scnInfoBuffer);

    // Default parameters
    m_shadowParams = {0.8f, 1.f, 0.f};

    m_VclipMin = {-FLT_MAX};
    m_VclipMax = {FLT_MAX};
    m_Vleaf = {0.f};
    m_Vframes = {0.f};

    m_transferVec = {};

    m_Vthreshold = {0.1f, 0.f, 1.f};
    m_extinct = {-1.1f, 1.5f, 0.f};
    m_steps = {1.f, 16.f, 0.1f};
    m_cutoff = {0.005f, 0.01f, 0.f};
    m_backgroundColor = dm::float4{0.f, 0.f, 0.f, 1.f};

    m_sectionPoint = dm::float3::zero();
    m_sectionNormal = {0.f, 1.f, 0.f};

    m_shading = VolShadeType::VOXEL;
    m_filterMode = 0;
    m_frame = 0;
    m_sample = 0;
    m_usedGVDBChannel = -1;
    m_epsilon = 1e-3f;

    m_rayNormalBias = 0.f;

    setTransform(dm::float3::zero(), dm::float3{1.f}, dm::float3::zero(),
                 dm::float3::zero());
}

VolRenderer::~VolRenderer() {}

void VolRenderer::setLinearTransferFunc(float t0, float t1, dm::float4 v0, dm::float4 v1) {
    const int sz = 16384;
    int n0 = static_cast<int>(t0 * sz);
    int n1 = static_cast<int>(t1 * sz);

    if (m_transferFunc.empty()) m_transferFunc.resize(sz);

    for (int n = n0; n < n1; ++n) {
        float t = float(n - n0) / float(n1 - n0);
        dm::float4 clr = dm::lerp(v0, v1, t);
        m_transferFunc[n] = clr;
    }
}

void VolRenderer::commitTransferFunc() {
    gp::BufferDesc desc;
    desc.byteSize = m_transferFunc.size() * sizeof(dm::float4);
    UT_V_GP(m_device->createBuffer(desc, &m_transferFuncGPU));

    m_queue->writeBuffer(m_transferFuncGPU, m_transferFunc.data(), desc.byteSize, 0);
}

void VolRenderer::setSteps(float directStep, float shadowStep, float fineStep) {
    m_steps = {directStep, shadowStep, fineStep};
}

void VolRenderer::setVolumeRange(float viso, float vmin, float vmax) {
    m_Vthreshold = {viso, vmin, vmax};
}

void VolRenderer::setExtinct(float a, float b, float c) {
    m_extinct = {a, b, c};
}

void VolRenderer::setCutoff(float a, float b, float c) {
    m_cutoff = {a, b, c};
}

void VolRenderer::setBackgroundColor(dm::float4 color) {
    m_backgroundColor = color;
}

void VolRenderer::setCrossSection(dm::float3 pos, dm::float3 norm) {
    m_sectionPoint = pos;
    m_sectionNormal = norm;
}

void VolRenderer::setShading(VolShadeType s) { m_shading = s; }

void VolRenderer::setShadowParams(float x, float y, float z) {
    m_shadowParams = {x, y, z};
}

void VolRenderer::setSample(int s) { m_sample = s; }

void VolRenderer::setFrame(int f) { m_frame = f; }

void VolRenderer::setFilterMode(int f) { m_filterMode = f; }

void VolRenderer::setRayNormalBias(float bias) { m_rayNormalBias = bias; }

void SampleUtils::VolRenderer::setUsedGVDBChannel(int chann) { m_usedGVDBChannel = chann; }

void SampleUtils::VolRenderer::setEpsilon(float eps) { m_epsilon = eps; }

void VolRenderer::setTransform(dm::float3 preTrans, dm::float3 scale, dm::float3 angles,
                                dm::float3 trans) {
    m_Pretrans = preTrans; // preT
    m_Scale = scale; // S
    m_Angles = angles; // R
    m_Trans = trans; // T

    dm::affine3 rot = dm::rotationQuat(m_Angles).toAffine();

    // (S R)^{-1}
    m_InvRot = dm::affineToHomogeneous(inverse(dm::scaling(m_Scale) * rot));

    // PT S R T
    m_Xform = dm::affineToHomogeneous(dm::translation(m_Pretrans) * dm::scaling(m_Scale) *
                                      rot * dm::translation(trans));
    m_InvXform = dm::inverse(m_Xform);
}

void VolRenderer::setDepthBuffer(donut::gp::IBuffer* depthBuffer) {
    m_depthBuffer = depthBuffer;
}

void VolRenderer::render(gvdb::GVDB* pGVDB, int channel,
                         donut::gp::IBuffer* pRenderBuffer, Camera* pCamera, int w,
                         int h, Camera* pLight) {
    prepareRender(pCamera, w, h, pLight);

    gp::dim3 blockDim{16, 16, 1};
    gp::dim3 gridDim{dm::div_ceil(w, blockDim.x), dm::div_ceil(h, blockDim.y), 1};

    gp::IKernel* pKernel;
    switch (m_shading) {
        case VolShadeType::VOXEL:
            pKernel = m_voxelKernel;
            break;
        case VolShadeType::TRILINEAR:
            pKernel = m_trilinearKernel;
            break;
        case VolShadeType::TRICUBIC:
            pKernel = m_tricubicKernel;
            break;
        case VolShadeType::EMPTYSKIP:
            pKernel = m_emptySkipKernel;
            break;
        case VolShadeType::SECTION2D:
            pKernel = m_section2DKernel;
            break;
        case VolShadeType::SECTION3D:
            pKernel = m_section3DKernel;
            break;
        case VolShadeType::LEVELSET:
            pKernel = m_levelsetKernel;
            break;
        case VolShadeType::VOLUME:
            pKernel = m_rayDeepKernel;
            break;
    }

    gp::KernelArg args[] = {
        gp::KernelArg::Buffer(pGVDB->getVBDInfoGPU()),
        gp::KernelArg::Scalar(channel),
        gp::KernelArg::Buffer(pRenderBuffer)
    };

    UT_V_GP(m_queue->setConstantBuffer2(pKernel, "scn", m_scnInfoBuffer, 0));
    UT_V_GP(m_queue->launch(pKernel, gridDim, blockDim, args, std::size(args)));

    UT_V_GP(m_device->waitForQueue(m_queue));
}

const dm::float4x4& VolRenderer::getTransform() { return m_Xform; }

void VolRenderer::prepareRender(Camera* pCamera, int w, int h, Camera *pLight) {
    // Send scene data
    dm::float3 rayOrigin;
    dm::float4 rayDirTL, rayDirTR, rayDirBL, rayDirBR;

    float zNear = pCamera->getZNear();
    rayOrigin = pCamera->getCameraPos();
    auto matViewInvNoT = pCamera->getViewMatrixInv();
    matViewInvNoT[3] = dm::float4{0.f, 0.f, 0.f, 1.f};
    auto matViewProjInv = dm::inverse(pCamera->getProjMatrix()) * matViewInvNoT;
    rayDirTL = dm::float4{-1.f, 1.f, zNear, 1.f} * matViewProjInv;
    rayDirTL /= rayDirTL.w;
    rayDirTR = dm::float4{1.f, 1.f, zNear, 1.f} * matViewProjInv;
    rayDirTR /= rayDirTR.w;
    rayDirBL = dm::float4{-1.f, -1.f, zNear, 1.f} * matViewProjInv;
    rayDirBL /= rayDirBL.w;
    rayDirBR = dm::float4{1.f, -1.f, zNear, 1.f} * matViewProjInv;
    rayDirBR /= rayDirBR.w;

    ScnInfo scnInfo;
    scnInfo.width = w;
    scnInfo.height = h;
    scnInfo.camnear = pCamera->getZNear();
    scnInfo.camfar = pCamera->getZFar();
    scnInfo.campos = rayOrigin;
    scnInfo.cams = rayDirTL;
    scnInfo.camu = (rayDirTR - rayDirTL).xyz();
    scnInfo.camv = (rayDirBL - rayDirTL).xyz();
    scnInfo.bias = m_rayNormalBias;

    dm::float4 lightPos4 = dm::float4{ pLight->getCameraPos(), 1.f};
    lightPos4 = lightPos4 * m_InvXform;
    scnInfo.light_pos = lightPos4.xyz();
    scnInfo.slice_pnt = m_sectionPoint;
    scnInfo.slice_norm = m_sectionNormal;
    scnInfo.shading = (char)m_shading;
    scnInfo.filtering = m_filterMode;
    scnInfo.frame = m_frame;
    scnInfo.samples = m_sample;
    scnInfo.shadow_params = m_shadowParams;
    scnInfo.backclr = m_backgroundColor;
    scnInfo.extinct = m_extinct;
    scnInfo.steps = m_steps;
    scnInfo.cutoff = m_cutoff;
    scnInfo.thresh = m_Vthreshold;
    scnInfo.gvdb_channel = m_usedGVDBChannel;
    scnInfo.epsilon = m_epsilon;

    // Grid transform
    memcpy(&scnInfo.xform, &m_Xform, sizeof(m_Xform));
    memcpy(&scnInfo.invxform, &m_InvXform, sizeof(m_InvXform));
    memcpy(&scnInfo.invxrot, &m_InvRot, sizeof(m_InvRot));

    // Transfer function
    scnInfo.transfer = m_transferFuncGPU->getNativeHandle();
    DONUT_ASSERT(scnInfo.transfer != 0);

    scnInfo.outbuf = -1;  // NOT USED
    // Depth buffer
    scnInfo.dbuf = m_depthBuffer ? m_depthBuffer->getNativeHandle() : 0;

    UT_V_GP(m_queue->writeBuffer(m_scnInfoBuffer, &scnInfo, sizeof(scnInfo), 0));
}

}  // namespace gvdb