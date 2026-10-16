#include "scene.h"

#include <DirectXColors.h>

#include <algorithm>
#include <cmath>

namespace MtScene
{

namespace
{

// Default view parameters of the sample
const XMVECTORF32 s_vDefaultEye    = { 30.0f, 150.0f, -150.0f, 0.f };
const XMVECTORF32 s_vDefaultLookAt = { 0.0f, 60.0f, 0.0f, 0.f };
constexpr float s_fNearPlane  = 2.0f;
constexpr float s_fFarPlane   = 4000.0f;
constexpr float s_fFOV        = XM_PI / 4.0f;
const XMVECTORF32 s_vSceneCenter = { 0.0f, 350.0f, 0.0f, 0.f };
constexpr float s_fSceneRadius        = 600.0f;
constexpr float s_fDefaultCameraRadius = 300.0f;
constexpr float s_fMinCameraRadius     = 150.0f;
constexpr float s_fMaxCameraRadius     = 450.0f;

const XMVECTORF32 s_vAmbientColor = { 0.04f * 0.760f, 0.04f * 0.793f, 0.04f * 0.822f, 1.000f };
const XMVECTORF32 s_vMirrorTint   = { 0.3f, 0.5f, 1.0f, 1.0f };

// Benchmark camera: a slow orbit around the look-at point, deterministic in time.
constexpr float s_fScriptedYawSpeed = 0.25f; // radians per second

} // namespace

const XMFLOAT4 Scene::kClearColor = XMFLOAT4(0.098039225f, 0.098039225f, 0.439215720f, 1.f); // Colors::MidnightBlue

const char* ModeName(Mode mode)
{
    switch (mode)
    {
        case Mode::SingleThreaded: return "st";
        case Mode::PerScene: return "mt_scene";
        case Mode::PerChunk: return "mt_chunk";
    }
    return "?";
}

bool ParseMode(const std::string& name, Mode& mode)
{
    if (name == "st") { mode = Mode::SingleThreaded; return true; }
    if (name == "mt_scene") { mode = Mode::PerScene; return true; }
    if (name == "mt_chunk") { mode = Mode::PerChunk; return true; }
    return false;
}

Scene::Scene()
{
    XMStoreFloat3(&m_Target, s_vDefaultLookAt);
    // CModelViewerCamera::SetViewParams + SetRadius(300): orbit the look-at point at 300 units along
    // the default eye direction.
    const XMVECTOR offset = XMVectorSubtract(s_vDefaultEye, s_vDefaultLookAt);
    const float x = XMVectorGetX(offset), y = XMVectorGetY(offset), z = XMVectorGetZ(offset);
    const float len = sqrtf(x * x + y * y + z * z);
    m_Yaw = atan2f(x, z);
    m_Pitch = asinf(y / len);
    m_Distance = s_fDefaultCameraRadius;

    BuildMirrors();
    InitLights();
}

void Scene::SetViewport(uint32_t width, uint32_t height)
{
    m_Width = std::max(1u, width);
    m_Height = std::max(1u, height);
}

void Scene::Orbit(float yawDelta, float pitchDelta)
{
    m_Yaw += yawDelta;
    m_Pitch = std::clamp(m_Pitch + pitchDelta, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
}

void Scene::Zoom(float distanceDelta)
{
    m_Distance = std::clamp(m_Distance + distanceDelta, s_fMinCameraRadius, s_fMaxCameraRadius);
}

XMVECTOR Scene::EyePosition() const
{
    const float cp = cosf(m_Pitch);
    const XMVECTOR dir = XMVectorSet(cp * sinf(m_Yaw), sinf(m_Pitch), cp * cosf(m_Yaw), 0.f);
    return XMVectorAdd(XMLoadFloat3(&m_Target), XMVectorScale(dir, m_Distance));
}

XMMATRIX Scene::ViewProj() const
{
    const XMMATRIX view = XMMatrixLookAtLH(EyePosition(), XMLoadFloat3(&m_Target), g_XMIdentityR1);
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(s_fFOV, float(m_Width) / float(m_Height), s_fNearPlane, s_fFarPlane);
    return view * proj;
}

void Scene::BuildMirrors()
{
    // Hard-coded from the sdkmesh contents (the sample's CreateMirrorModels).
    const XMVECTORF32 s_vMirrorCenter[] = {
        { -35.1688f, 89.279683f, -0.7488765f, 0.0f },
        { 41.2174f, 89.279683f, -0.7488745f, 0.0f },
        { 3.024275f, 89.279683f, -54.344299f, 0.0f },
        { 3.02427475f, 89.279683f, 52.8466f, 0.0f } };
    const XMVECTORF32 s_vMirrorNormal[] = {
        { -0.998638464f, -0.052165297f, 0.0f, 0.0f },
        { 0.998638407f, -0.052166381f, 3.15017E-08f, 0.0f },
        { 0.0f, -0.076278878f, -0.997086522f, 0.0f },
        { -5.22129E-08f, -0.076279957f, 0.99708644f, 0.0f } };
    const XMVECTORF32 s_vMirrorDims[] = {
        { 104.190899f, 92.19923178f, 0.0f, 0.0f },
        { 104.190895f, 92.19922656f, 0.0f, 0.0f },
        { 76.3862f, 92.3427325f, 0.0f, 0.0f },
        { 76.386196f, 92.34274043f, 0.0f, 0.0f } };
    const XMVECTORF32 vOffsets[] = {
        { -.5f, .5f, .0f, .0f },
        { .5f, .5f, .0f, .0f },
        { -.5f, -.5f, .0f, .0f },
        { .5f, -.5f, .0f, .0f } };

    for (int i = 0; i < kNumMirrors; ++i)
    {
        Mirror& m = m_Mirrors[i];
        for (int j = 0; j < 4; ++j)
        {
            MirrorVertex& v = m.vertices[j];
            XMStoreFloat3(&v.position, XMVectorMultiply(s_vMirrorDims[i], vOffsets[j]));
            v.normal = XMFLOAT3(0.f, 0.f, 0.f);
            v.texcoord = XMFLOAT2(0.f, 0.f);
            v.tangent = XMFLOAT3(0.f, 0.f, 0.f);
        }

        // Mirror local-to-world: Z along -normal, Y up.
        XMVECTOR R2 = XMVector3Normalize(XMVectorNegate(s_vMirrorNormal[i]));
        XMVECTOR R1 = g_XMIdentityR1;
        XMVECTOR R0 = XMVector3Normalize(XMVector3Cross(R1, R2));
        R1 = XMVector3Cross(R2, R0);
        XMMATRIX W;
        W.r[0] = R0;
        W.r[1] = R1;
        W.r[2] = R2;
        W.r[3] = XMVectorSetW(s_vMirrorCenter[i], 1.0f);
        XMStoreFloat4x4(&m.world, W);
        XMStoreFloat4(&m.plane, XMPlaneFromPointNormal(s_vMirrorCenter[i], s_vMirrorNormal[i]));
    }
}

void Scene::InitLights()
{
    // Our hand-tuned approximation to the sky light
    const XMVECTORF32 s_lightDir0 = { -0.67f, -0.71f, +0.21f, 0.f };
    XMStoreFloat4(&m_LightColor[0], XMVectorSet(3.0f * 0.160f, 3.0f * 0.341f, 3.0f * 1.000f, 1.000f));
    const XMVECTOR dir0 = XMVector3Normalize(s_lightDir0);
    XMStoreFloat4(&m_LightDir[0], dir0);
    XMStoreFloat4(&m_LightPos[0], XMVectorSubtract(s_vSceneCenter, XMVectorScale(dir0, s_fSceneRadius)));
    m_LightFov[0] = XM_PI / 4.0f;

    // The three overhead lamps
    const XMVECTORF32 s_lightPos1 = { 0.0f, 400.0f, -250.0f, 0.f };
    const XMVECTORF32 s_lightPos2 = { 0.0f, 400.0f, 0.0f, 0.f };
    const XMVECTORF32 s_lightPos3 = { 0.0f, 400.0f, 250.0f, 0.f };

    XMStoreFloat4(&m_LightColor[1], XMVectorSet(0.4f * 0.895f, 0.4f * 0.634f, 0.4f * 0.626f, 1.0f));
    XMStoreFloat4(&m_LightPos[1], s_lightPos1);
    XMStoreFloat4(&m_LightDir[1], g_XMNegIdentityR1);
    m_LightFov[1] = 65.0f * (XM_PI / 180.0f);

    XMStoreFloat4(&m_LightColor[2], XMVectorSet(0.5f * 0.388f, 0.5f * 0.641f, 0.5f * 0.401f, 1.0f));
    XMStoreFloat4(&m_LightPos[2], s_lightPos2);
    XMStoreFloat4(&m_LightDir[2], g_XMNegIdentityR1);
    m_LightFov[2] = 65.0f * (XM_PI / 180.0f);

    XMStoreFloat4(&m_LightColor[3], XMVectorSet(0.4f * 1.000f, 0.4f * 0.837f, 0.4f * 0.848f, 1.0f));
    XMStoreFloat4(&m_LightPos[3], s_lightPos3);
    XMStoreFloat4(&m_LightDir[3], g_XMNegIdentityR1);
    m_LightFov[3] = 65.0f * (XM_PI / 180.0f);

    for (int iLight = 0; iLight < kNumLights; ++iLight)
    {
        m_LightAspect[iLight] = 1.0f;
        m_LightNear[iLight] = 100.f;
        m_LightFar[iLight] = 2.0f * s_fSceneRadius;
        m_LightFalloffDistEnd[iLight] = m_LightFar[iLight];
        m_LightFalloffDistRange[iLight] = 100.0f;
        m_LightFalloffCosAngleEnd[iLight] = cosf(m_LightFov[iLight] / 2.0f);
        m_LightFalloffCosAngleRange[iLight] = 0.1f;
    }
}

void Scene::Animate(float fTime, bool scriptedCamera)
{
    // Jigger the overhead lights --- hard-coded to indices 1,2,3 (as in the sample).
    XMVECTOR cycle1 = XMVectorSet(0.f, 0.f, 0.20f * sinf(2.0f * (fTime + 0.0f * XM_PI)), 0.f);
    XMStoreFloat4(&m_LightDir[1], XMVector3Normalize(XMVectorAdd(g_XMNegIdentityR1, cycle1)));

    XMVECTOR cycle2 = XMVectorSet(0.10f * cosf(1.6f * (fTime + 0.3f * XM_PI)), 0.f, 0.10f * sinf(1.6f * (fTime + 0.0f * XM_PI)), 0.f);
    XMStoreFloat4(&m_LightDir[2], XMVector3Normalize(XMVectorAdd(g_XMNegIdentityR1, cycle2)));

    XMVECTOR cycle3 = XMVectorSet(0.30f * cosf(2.4f * (fTime + 0.3f * XM_PI)), 0.f, 0.f, 0.f);
    XMStoreFloat4(&m_LightDir[3], XMVector3Normalize(XMVectorAdd(g_XMNegIdentityR1, cycle3)));

    if (scriptedCamera)
    {
        const XMVECTOR offset = XMVectorSubtract(s_vDefaultEye, s_vDefaultLookAt);
        const float x = XMVectorGetX(offset), z = XMVectorGetZ(offset);
        m_Yaw = atan2f(x, z) + s_fScriptedYawSpeed * fTime;
        m_Distance = s_fDefaultCameraRadius;
    }
}

XMMATRIX Scene::LightViewProj(int iLight) const
{
    const XMVECTOR vLightDir = XMLoadFloat4(&m_LightDir[iLight]);
    const XMVECTOR vLightPos = XMLoadFloat4(&m_LightPos[iLight]);
    const XMVECTOR vLookAt = XMVectorAdd(vLightPos, XMVectorScale(vLightDir, s_fSceneRadius));
    const XMMATRIX mLightView = XMMatrixLookAtLH(vLightPos, vLookAt, g_XMIdentityR1);
    const XMMATRIX mLightProj = XMMatrixPerspectiveFovLH(m_LightFov[iLight], m_LightAspect[iLight], m_LightNear[iLight], m_LightFar[iLight]);
    return mLightView * mLightProj;
}

void Scene::FillLightConstants(CbPerLight& out) const
{
    for (int iLight = 0; iLight < kNumLights; ++iLight)
    {
        LightData& l = out.lights[iLight];
        XMStoreFloat4x4(&l.viewProj, XMMatrixTranspose(LightViewProj(iLight)));
        l.position = m_LightPos[iLight];
        l.position.w = 1.0f;
        l.direction = m_LightDir[iLight];
        l.direction.w = 0.0f;
        l.color = m_LightColor[iLight];
        l.falloffs = XMFLOAT4(m_LightFalloffDistEnd[iLight], m_LightFalloffDistRange[iLight], m_LightFalloffCosAngleEnd[iLight],
                              m_LightFalloffCosAngleRange[iLight]);
    }
}

void Scene::FillScene(CbPerScene& out, FXMMATRIX viewProj, const XMFLOAT4& mirrorPlane, const XMFLOAT4& tint) const
{
    XMStoreFloat4x4(&out.viewProj, XMMatrixTranspose(viewProj));
    out.mirrorPlane = mirrorPlane;
    XMStoreFloat4(&out.ambientColor, s_vAmbientColor);
    out.tintColor = tint;
}

SceneDraw Scene::ShadowPass(int shadow) const
{
    SceneDraw d;
    d.kind = SceneKind::Shadow;
    FillScene(d.scene, LightViewProj(shadow), XMFLOAT4(0, 0, 0, 0), XMFLOAT4(1, 1, 1, 1));
    XMStoreFloat4x4(&d.object.world, XMMatrixIdentity());
    d.object.objectColor = XMFLOAT4(1, 1, 1, 1);
    d.hasLights = false;
    d.scissor = { 0, 0, LONG(kShadowMapSize), LONG(kShadowMapSize) };
    d.viewportWidth = d.viewportHeight = kShadowMapSize;
    return d;
}

SceneDraw Scene::DirectPass() const
{
    SceneDraw d;
    d.kind = SceneKind::Direct;
    FillScene(d.scene, ViewProj(), XMFLOAT4(0, 0, 0, 0), XMFLOAT4(1, 1, 1, 1));
    XMStoreFloat4x4(&d.object.world, XMMatrixIdentity());
    d.object.objectColor = XMFLOAT4(1, 1, 1, 1);
    d.hasLights = true;
    d.scissor = { 0, 0, LONG(m_Width), LONG(m_Height) };
    d.viewportWidth = m_Width;
    d.viewportHeight = m_Height;
    return d;
}

RECT Scene::MirrorScissor(int mirror, FXMMATRIX viewProj) const
{
    if (!m_TightMirrorScissor)
        return RECT{ 0, 0, LONG(m_Width), LONG(m_Height) };

    // Project the mirror corners to pixels and take their bounds (the sample's "tight mirror stencil
    // clipping space").
    const Mirror& m = m_Mirrors[mirror];
    const float w1 = float(m_Width) * 0.5f;
    const float h1 = float(m_Height) * 0.5f;
    const XMMATRIX matWVPS = XMLoadFloat4x4(&m.world) * viewProj *
        XMMatrixSet(w1, .0f, .0f, .0f, .0f, -h1, .0f, .0f, .0f, .0f, .0f, .0f, w1, h1, .0f, 1.0f);
    XMVECTOR vScMin = g_XMFltMax;
    XMVECTOR vScMax = g_XMFltMin;
    for (int i = 0; i < 4; ++i)
    {
        XMVECTOR c = XMVectorSetW(XMLoadFloat3(&m.vertices[i].position), 1.0f);
        c = XMVector4Transform(c, matWVPS);
        c = XMVectorDivide(c, XMVectorSplatW(c));
        vScMin = XMVectorMin(vScMin, c);
        vScMax = XMVectorMax(vScMax, c);
    }
    vScMin = XMVectorFloor(XMVectorMax(vScMin, g_XMZero));
    vScMax = XMVectorCeiling(XMVectorMin(vScMax, XMVectorSet(2.0f * w1, 2.0f * h1, 0.0, 0.0)));
    RECT r;
    r.left = LONG(XMVectorGetX(vScMin));
    r.right = LONG(XMVectorGetX(vScMax));
    r.top = LONG(XMVectorGetY(vScMin));
    r.bottom = LONG(XMVectorGetY(vScMax));
    if (r.right <= r.left || r.bottom <= r.top)
        r = RECT{ 0, 0, 0, 0 };
    return r;
}

MirrorPass Scene::MirrorPassParams(int mirror) const
{
    MirrorPass p;
    p.mirror = mirror;
    p.stencilBit = uint8_t(1u << (mirror % 8));

    const Mirror& m = m_Mirrors[mirror];
    const XMVECTOR plane = XMLoadFloat4(&m.plane);
    const XMVECTOR eye = EyePosition();
    // Back-facing mirrors are skipped entirely.
    p.visible = XMVectorGetX(XMPlaneDotCoord(plane, eye)) >= 0.0f;
    if (!p.visible)
        return p;

    const XMMATRIX viewProj = ViewProj();
    const XMMATRIX mirrorWorld = XMLoadFloat4x4(&m.world);
    const XMMATRIX reflect = XMMatrixReflect(plane);

    p.scissor = MirrorScissor(mirror, viewProj);

    XMStoreFloat4x4(&p.mirrorObject.world, XMMatrixTranspose(mirrorWorld));
    p.mirrorObject.objectColor = XMFLOAT4(1, 1, 1, 1);

    // The stencil and the mirror surface use the plain view-projection; the plane and the colours
    // are unused by those pipelines (vertex-only).
    FillScene(p.stencilScene, viewProj, XMFLOAT4(0, 0, 0, 0), XMFLOAT4(1, 1, 1, 1));

    // Depth clear: with the z row replaced by the w row, every mirror pixel gets z/w = 1.
    p.depthClearScene = p.stencilScene;
    XMFLOAT4X4& dc = p.depthClearScene.viewProj;
    dc._31 = dc._41; dc._32 = dc._42; dc._33 = dc._43; dc._34 = dc._44;

    // The reflected scene.
    p.reflected.kind = SceneKind::Mirrored;
    p.reflected.mirror = mirror;
    XMFLOAT4 tint;
    XMStoreFloat4(&tint, s_vMirrorTint);
    FillScene(p.reflected.scene, reflect * viewProj, m.plane, tint);
    XMStoreFloat4x4(&p.reflected.object.world, XMMatrixIdentity());
    p.reflected.object.objectColor = XMFLOAT4(1, 1, 1, 1);
    p.reflected.hasLights = true;
    p.reflected.scissor = p.scissor;
    p.reflected.viewportWidth = m_Width;
    p.reflected.viewportHeight = m_Height;
    return p;
}

} // namespace MtScene
