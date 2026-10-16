#pragma once
// The scene of the MultithreadedRendering sample without any graphics API: lights and their
// animation, the four mirrors, the camera (interactive orbit or the scripted benchmark path), the
// constant buffer layouts of mt_scene.hlsl and the per-pass parameters both renderers derive their
// draws from. Everything a frame renders is decided here so that the native and the nvrhi renderer
// issue identical work.

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h> // RECT
#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

#include "sdkmesh.h"

namespace MtScene
{

using namespace DirectX;

constexpr int kNumLights  = 4;
constexpr int kNumShadows = 1; // the first n lights cast shadows
constexpr int kNumMirrors = 4;
constexpr int kNumPasses  = kNumShadows + kNumMirrors + 1; // shadow maps, mirrors, direct view
constexpr uint32_t kShadowMapSize = 2048;                   // keep in sync with mt_scene.hlsl
constexpr uint32_t kMaxThreads = 64;

constexpr const wchar_t* kMeshFile = L"SquidRoom/SquidRoom.sdkmesh"; // under <bin>/../media

inline int PassShadow(int shadow) { return shadow; }
inline int PassMirror(int mirror) { return kNumShadows + mirror; }
constexpr int kPassDirect = kNumShadows + kNumMirrors;

enum class Mode
{
    SingleThreaded, // st
    PerScene,       // mt_scene
    PerChunk        // mt_chunk
};

const char* ModeName(Mode mode);
bool ParseMode(const std::string& name, Mode& mode);

// ---- constant buffers (mt_scene.hlsl; matrices stored transposed for the column-major default) ----

struct CbPerObject
{
    XMFLOAT4X4 world;
    XMFLOAT4   objectColor;
};

struct LightData
{
    XMFLOAT4X4 viewProj;
    XMFLOAT4   position;
    XMFLOAT4   direction;
    XMFLOAT4   color;
    XMFLOAT4   falloffs; // x = dist end, y = dist range, z = cos angle end, w = cos range
};

struct CbPerLight
{
    LightData lights[kNumLights];
};

struct CbPerScene
{
    XMFLOAT4X4 viewProj;
    XMFLOAT4   mirrorPlane;
    XMFLOAT4   ambientColor;
    XMFLOAT4   tintColor;
};

// ---- geometry of the mirrors (the "uncompressed" vertex layout of the sample: 44 bytes) ----

struct MirrorVertex
{
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT2 texcoord;
    XMFLOAT3 tangent;
};

struct Mirror
{
    MirrorVertex vertices[4]; // triangle strip
    XMFLOAT4X4   world;
    XMFLOAT4     plane;
};

// The scene pass kinds (which pipeline family a RenderScene call uses).
enum class SceneKind
{
    Shadow,   // depth only into a shadow map
    Mirrored, // the reflected scene inside a mirror's stencil area
    Direct    // the regular view
};

// Everything one "render the whole model" call needs. Filled by Scene, consumed by both renderers.
struct SceneDraw
{
    SceneKind  kind = SceneKind::Direct;
    int        mirror = -1;        // Mirrored: which stencil bit / pipeline variant
    CbPerScene scene = {};         // transposed matrices, ready for the GPU
    CbPerObject object = {};       // identity world, white
    bool       hasLights = false;  // Shadow passes bind no light data
    RECT       scissor = {};       // pixels
    uint32_t   viewportWidth = 0, viewportHeight = 0;
};

// The extra draws of a mirror pass (the stencil quad, the depth clear, the mirror surface).
struct MirrorPass
{
    bool        visible = false;
    int         mirror = 0;
    uint8_t     stencilBit = 1;
    CbPerObject mirrorObject = {};     // world = mirror matrix
    CbPerScene  stencilScene = {};     // viewProj
    CbPerScene  depthClearScene = {};  // viewProj with z := w (clears depth to 1 inside the stencil area)
    RECT        scissor = {};
    SceneDraw   reflected;             // the scene seen in the mirror
};

class Scene
{
public:
    Scene();

    void SetViewport(uint32_t width, uint32_t height);
    uint32_t ViewportWidth() const { return m_Width; }
    uint32_t ViewportHeight() const { return m_Height; }

    // Lights swing and, in benchmark mode, the camera orbits; `time` is absolute seconds.
    void Animate(float time, bool scriptedCamera);

    // Interactive camera (ignored while the camera is scripted).
    void Orbit(float yawDelta, float pitchDelta);
    void Zoom(float distanceDelta);

    // Whether the mirror's stencil area is reduced to the projected mirror bounds (the sample's
    // "tight mirror stencil clipping space" option; off by default like the sample's UI).
    void SetTightMirrorScissor(bool enable) { m_TightMirrorScissor = enable; }
    bool TightMirrorScissor() const { return m_TightMirrorScissor; }

    const Mirror& GetMirror(int i) const { return m_Mirrors[i]; }
    XMVECTOR EyePosition() const;
    XMMATRIX ViewProj() const;

    // Per-pass parameters of the current frame.
    SceneDraw  ShadowPass(int shadow) const;
    MirrorPass MirrorPassParams(int mirror) const;
    SceneDraw  DirectPass() const;
    void       FillLightConstants(CbPerLight& out) const;

    static const XMFLOAT4 kClearColor; // the sample clears to MidnightBlue

private:
    void BuildMirrors();
    void InitLights();
    XMMATRIX LightViewProj(int light) const;
    void FillScene(CbPerScene& out, FXMMATRIX viewProj, const XMFLOAT4& mirrorPlane, const XMFLOAT4& tint) const;
    RECT MirrorScissor(int mirror, FXMMATRIX viewProj) const;

    uint32_t m_Width = 1, m_Height = 1;
    bool m_TightMirrorScissor = false;

    // camera: orbit around the target
    XMFLOAT3 m_Target;
    float m_Yaw = 0.f, m_Pitch = 0.f, m_Distance = 300.f;

    Mirror m_Mirrors[kNumMirrors];

    XMFLOAT4 m_LightColor[kNumLights];
    XMFLOAT4 m_LightPos[kNumLights];
    XMFLOAT4 m_LightDir[kNumLights];
    float m_LightFov[kNumLights];
    float m_LightAspect[kNumLights];
    float m_LightNear[kNumLights];
    float m_LightFar[kNumLights];
    float m_LightFalloffDistEnd[kNumLights];
    float m_LightFalloffDistRange[kNumLights];
    float m_LightFalloffCosAngleEnd[kNumLights];
    float m_LightFalloffCosAngleRange[kNumLights];
};

// mt_chunk: mesh `drawIndex` of the render order belongs to chunk `chunk` of `chunkCount`.
inline bool ChunkOwnsDraw(uint32_t drawIndex, uint32_t chunk, uint32_t chunkCount)
{
    return drawIndex % chunkCount == chunk;
}

} // namespace MtScene
