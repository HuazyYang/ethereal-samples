/*
* Hello DXR -- a Donut / nvrhi port of D3D12-Trial/HelloDXR.
*
* Three spinning tetrahedra, a ground plane and a Menger sponge, ray traced with one primary ray per
* pixel and one shadow ray per hit towards a point light that orbits the scene. The top-level
* acceleration structure is refitted every frame.
*
* What changed in the port:
*  - The original used DXR local root signatures: a shader record per instance carried that instance's
*    vertex/index buffer addresses and normal matrix. nvrhi exposes local bindings on D3D12 only, so
*    every mesh now lives in one vertex and one index buffer, a StructuredBuffer<InstanceData> holds the
*    per-instance offsets and normal matrix, and the closest-hit shader indexes it with InstanceIndex().
*    The whole scene is bound through a single global binding layout; the same code runs on Vulkan.
*  - The hand-rolled SBT, descriptor heap and frame-resource ring are nvrhi's shader table, binding
*    set and volatile constant buffer.
*  - DXUT's CModelViewerCamera is Donut's ThirdPersonCamera (left mouse orbits, wheel zooms).
*/

#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/math/math.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ShaderFactory.h>
#include <nvrhi/utils.h>

#include <cstdlib>
#include <memory>
#include <vector>

using namespace donut;
using namespace donut::math;

static const char* g_WindowTitle = "Hello DXR (Donut / nvrhi)";

namespace
{

// Must match hello_dxr.hlsl.
struct FrameConstants
{
    float4x4 clipToWorld;
    float4   eyePosition;
    float4   lightPosition;
    float4   lightStrength;
};

struct InstanceData
{
    float4x4 normalMatrix;
    uint32_t vertexOffset;
    uint32_t indexOffset;
    uint32_t padding[2];
};

// 16-byte members only: the same layout for a D3D12 structured buffer and a Vulkan storage buffer
// (std430 would pad float3 members to 16 bytes).
struct Vertex
{
    float4 position; // xyz
    float4 normal;   // xyz
    float4 color;
};

struct Mesh
{
    uint32_t vertexOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
};

struct Instance
{
    uint32_t mesh = 0;
    affine3  transform = affine3::identity();
};

// The procedural geometry of the original sample.
class Geometry
{
public:
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    std::vector<Mesh>     meshes;

    // A tetrahedron with flat normals: every face gets its own three vertices.
    uint32_t AddTetrahedron()
    {
        const float s89 = sqrtf(8.f / 9.f), s29 = sqrtf(2.f / 9.f), s23 = sqrtf(2.f / 3.f);
        const float3 corners[4] = {
            float3(s89, 0.f, -1.f / 3.f),
            float3(-s29, s23, -1.f / 3.f),
            float3(-s29, -s23, -1.f / 3.f),
            float3(0.f, 0.f, 1.f),
        };
        const float4 colors[4] = {
            float4(1.f, 0.f, 0.f, 1.f), float4(0.f, 1.f, 0.f, 1.f), float4(0.f, 0.f, 1.f, 1.f), float4(1.f, 0.f, 1.f, 1.f)
        };
        const uint32_t faces[12] = { 0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2 };

        Mesh mesh;
        mesh.vertexOffset = uint32_t(vertices.size());
        mesh.indexOffset = uint32_t(indices.size());
        for (uint32_t f = 0; f < 12; f += 3)
        {
            const float3 p0 = corners[faces[f]], p1 = corners[faces[f + 1]], p2 = corners[faces[f + 2]];
            const float3 n = normalize(cross(p0 - p1, p2 - p1));
            vertices.push_back({ float4(p0, 1.f), float4(n, 0.f), colors[faces[f]] });
            vertices.push_back({ float4(p1, 1.f), float4(n, 0.f), colors[faces[f + 1]] });
            vertices.push_back({ float4(p2, 1.f), float4(n, 0.f), colors[faces[f + 2]] });
            indices.push_back(f);
            indices.push_back(f + 1);
            indices.push_back(f + 2);
        }
        mesh.vertexCount = 12;
        mesh.indexCount = 12;
        meshes.push_back(mesh);
        return uint32_t(meshes.size() - 1);
    }

    // A 2x2 quad in the XZ plane facing +Y, light grey.
    uint32_t AddPlane()
    {
        Mesh mesh;
        mesh.vertexOffset = uint32_t(vertices.size());
        mesh.indexOffset = uint32_t(indices.size());
        const float4 grey(3.f, 3.f, 3.f, 1.f);
        const float4 up(0.f, 1.f, 0.f, 0.f);
        vertices.push_back({ float4(-1.f, 0.f, -1.f, 1.f), up, grey });
        vertices.push_back({ float4(-1.f, 0.f, 1.f, 1.f), up, grey });
        vertices.push_back({ float4(1.f, 0.f, 1.f, 1.f), up, grey });
        vertices.push_back({ float4(1.f, 0.f, -1.f, 1.f), up, grey });
        const uint32_t quad[6] = { 0, 1, 2, 0, 2, 3 };
        indices.insert(indices.end(), quad, quad + 6);
        mesh.vertexCount = 4;
        mesh.indexCount = 6;
        meshes.push_back(mesh);
        return uint32_t(meshes.size() - 1);
    }

    // The randomised Menger sponge of the original (GenerateMengerSponge(4, 0.75f)): every level keeps each
    // sub-cube with probability 20/27, so about the volume of the regular sponge, but irregular.
    uint32_t AddMengerSponge(int levels)
    {
        struct Cube { float3 corner; float size; };
        std::vector<Cube> current = { { float3(-0.5f), 1.f } };
        std::vector<Cube> next;
        uint32_t rng = 0x2545F491u; // fixed seed: the same sponge on every run and platform
        auto random01 = [&rng]() {
            rng = rng * 1664525u + 1013904223u;
            return float(rng >> 8) / float(1u << 24);
        };
        for (int level = 0; level < levels; ++level)
        {
            next.clear();
            for (const Cube& c : current)
            {
                const float size = c.size / 3.f;
                for (int x = 0; x < 3; x++)
                    for (int y = 0; y < 3; y++)
                        for (int z = 0; z < 3; z++)
                            if (random01() <= 20.f / 27.f)
                                next.push_back({ c.corner + float3(float(x), float(y), float(z)) * size, size });
            }
            std::swap(current, next);
        }

        Mesh mesh;
        mesh.vertexOffset = uint32_t(vertices.size());
        mesh.indexOffset = uint32_t(indices.size());
        const float4 red(1.f, 0.f, 0.f, 1.f);
        auto quad = [&](const float3& origin, const float3& dx, const float3& dy, bool flip) {
            const uint32_t base = uint32_t(vertices.size() - mesh.vertexOffset);
            float3 n = cross(normalize(dy), normalize(dx));
            if (flip)
            {
                n = -n;
                const uint32_t idx[6] = { base, base + 2, base + 1, base + 3, base + 1, base + 2 };
                indices.insert(indices.end(), idx, idx + 6);
            }
            else
            {
                const uint32_t idx[6] = { base, base + 1, base + 2, base + 2, base + 1, base + 3 };
                indices.insert(indices.end(), idx, idx + 6);
            }
            const float4 n4(n, 0.f);
            vertices.push_back({ float4(origin, 1.f), n4, red });
            vertices.push_back({ float4(origin + dx, 1.f), n4, red });
            vertices.push_back({ float4(origin + dy, 1.f), n4, red });
            vertices.push_back({ float4(origin + dx + dy, 1.f), n4, red });
        };
        for (const Cube& c : current)
        {
            const float s = c.size;
            const float3 o = c.corner;
            quad(o, float3(s, 0, 0), float3(0, s, 0), false);
            quad(o, float3(s, 0, 0), float3(0, 0, s), true);
            quad(o, float3(0, s, 0), float3(0, 0, s), false);
            const float3 farCorner = o + float3(s);
            quad(farCorner, float3(-s, 0, 0), float3(0, -s, 0), true);
            quad(farCorner, float3(-s, 0, 0), float3(0, 0, -s), false);
            quad(farCorner, float3(0, -s, 0), float3(0, 0, -s), true);
        }
        mesh.vertexCount = uint32_t(vertices.size()) - mesh.vertexOffset;
        mesh.indexCount = uint32_t(indices.size()) - mesh.indexOffset;
        meshes.push_back(mesh);
        return uint32_t(meshes.size() - 1);
    }
};

} // namespace

class HelloDXR : public app::IRenderPass
{
    NVRHI_INHERIT_INTERFACE_TABLE()
private:
    nvrhi::AutoPtr<vfs::RootFileSystem> m_RootFS;
    nvrhi::AutoPtr<engine::ShaderFactory> m_ShaderFactory;
    nvrhi::AutoPtr<engine::CommonRenderPasses> m_CommonPasses;
    std::unique_ptr<engine::BindingCache> m_BindingCache;

    nvrhi::CommandListHandle m_CommandList;
    nvrhi::ShaderLibraryHandle m_ShaderLibrary;
    nvrhi::rt::PipelineHandle m_Pipeline;
    nvrhi::rt::ShaderTableHandle m_ShaderTable;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingSetHandle m_BindingSet;

    nvrhi::BufferHandle m_FrameConstants;
    nvrhi::BufferHandle m_VertexBuffer;
    nvrhi::BufferHandle m_IndexBuffer;
    nvrhi::BufferHandle m_InstanceBuffer;
    std::vector<nvrhi::rt::AccelStructHandle> m_BottomLevelAS;
    nvrhi::rt::AccelStructHandle m_TopLevelAS;
    nvrhi::TextureHandle m_Output;

    Geometry m_Geometry;
    std::vector<Instance> m_Instances;
    std::vector<nvrhi::rt::InstanceDesc> m_InstanceDescs;
    std::vector<InstanceData> m_InstanceData;
    uint32_t m_TetrahedronMesh = 0;

    app::ThirdPersonCamera m_Camera;
    float m_Time = 0.f;
    float m_LightAngle = 0.f;

public:
    using IRenderPass::IRenderPass;

    bool Init()
    {
        const std::filesystem::path exeDir = app::GetDirectoryWithExecutable();
        const char* shaderType = app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        m_RootFS = MAKE_RC_OBJ_PTR(vfs::RootFileSystem);
        m_RootFS->mount("/shaders/donut", exeDir / "shaders/framework" / shaderType);
        m_RootFS->mount("/shaders/app", exeDir / "shaders/hello_dxr" / shaderType);

        m_ShaderFactory = MAKE_RC_OBJ_PTR(engine::ShaderFactory, GetDevice(), m_RootFS, "/shaders");
        m_CommonPasses = MAKE_RC_OBJ_PTR(engine::CommonRenderPasses, GetDevice(), m_ShaderFactory);
        m_BindingCache = std::make_unique<engine::BindingCache>(GetDevice());

        m_Camera.LookAt(float3(0.f, 0.f, -10.f), float3(0.f, 0.f, 0.f));
        m_Camera.SetMaxDistance(100.f);

        GetDevice()->createBuffer(
            nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(FrameConstants), "FrameConstants", 16), &m_FrameConstants);

        if (!CreateScene())
            return false;
        if (!CreateRayTracingPipeline())
            return false;

        GetDevice()->createCommandList(nvrhi::CommandListParameters(), &m_CommandList);
        m_CommandList->open();
        UploadGeometry(m_CommandList);
        CreateAccelStructs(m_CommandList);
        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);
        GetDevice()->waitForIdle();
        return true;
    }

    bool CreateScene()
    {
        m_TetrahedronMesh = m_Geometry.AddTetrahedron();
        const uint32_t planeMesh = m_Geometry.AddPlane();
        const uint32_t mengerMesh = m_Geometry.AddMengerSponge(4);

        // Three tetrahedra (animated in Animate()), the plane and the sponge.
        for (int i = 0; i < 3; ++i)
            m_Instances.push_back({ m_TetrahedronMesh, affine3::identity() });
        m_Instances.push_back({ planeMesh, translation(float3(0.f, -1.f, 0.f)) * scaling(float3(6.f, 1.f, 6.f)) });
        m_Instances.push_back({ mengerMesh, scaling(float3(2.f)) * translation(float3(0.f, 0.f, 2.f)) });
        AnimateInstances();

        m_InstanceDescs.resize(m_Instances.size());
        m_InstanceData.resize(m_Instances.size());
        return true;
    }

    void UploadGeometry(nvrhi::ICommandList* commandList)
    {
        nvrhi::BufferDesc vbDesc;
        vbDesc.byteSize = m_Geometry.vertices.size() * sizeof(Vertex);
        vbDesc.structStride = sizeof(Vertex);
        vbDesc.isAccelStructBuildInput = true;
        vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
        vbDesc.debugName = "Vertices";
        GetDevice()->createBuffer(vbDesc, &m_VertexBuffer);

        nvrhi::BufferDesc ibDesc;
        ibDesc.byteSize = m_Geometry.indices.size() * sizeof(uint32_t);
        ibDesc.structStride = sizeof(uint32_t);
        ibDesc.isAccelStructBuildInput = true;
        ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
        ibDesc.debugName = "Indices";
        GetDevice()->createBuffer(ibDesc, &m_IndexBuffer);

        const nvrhi::ResourceStates readState = nvrhi::ResourceStates::ShaderResource | nvrhi::ResourceStates::AccelStructBuildInput;
        commandList->beginTrackingBufferState(m_VertexBuffer, nvrhi::ResourceStates::CopyDest);
        commandList->writeBuffer(m_VertexBuffer, m_Geometry.vertices.data(), vbDesc.byteSize);
        commandList->setPermanentBufferState(m_VertexBuffer, readState);
        commandList->beginTrackingBufferState(m_IndexBuffer, nvrhi::ResourceStates::CopyDest);
        commandList->writeBuffer(m_IndexBuffer, m_Geometry.indices.data(), ibDesc.byteSize);
        commandList->setPermanentBufferState(m_IndexBuffer, readState);

        // Rewritten every frame (the tetrahedra move): nvrhi uploads through its upload manager.
        nvrhi::BufferDesc instDesc;
        instDesc.byteSize = m_InstanceData.size() * sizeof(InstanceData);
        instDesc.structStride = sizeof(InstanceData);
        instDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        instDesc.keepInitialState = true;
        instDesc.debugName = "InstanceData";
        GetDevice()->createBuffer(instDesc, &m_InstanceBuffer);
    }

    void CreateAccelStructs(nvrhi::ICommandList* commandList)
    {
        for (const Mesh& mesh : m_Geometry.meshes)
        {
            nvrhi::rt::GeometryDesc geometry;
            auto& triangles = geometry.geometryData.triangles;
            triangles.vertexBuffer = m_VertexBuffer;
            triangles.vertexOffset = uint64_t(mesh.vertexOffset) * sizeof(Vertex);
            triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
            triangles.vertexStride = sizeof(Vertex);
            triangles.vertexCount = mesh.vertexCount;
            triangles.indexBuffer = m_IndexBuffer;
            triangles.indexOffset = uint64_t(mesh.indexOffset) * sizeof(uint32_t);
            triangles.indexFormat = nvrhi::Format::R32_UINT;
            triangles.indexCount = mesh.indexCount;
            geometry.geometryType = nvrhi::rt::GeometryType::Triangles;
            geometry.flags = nvrhi::rt::GeometryFlags::Opaque;

            nvrhi::rt::AccelStructDesc blasDesc;
            blasDesc.isTopLevel = false;
            blasDesc.bottomLevelGeometries.push_back(geometry);
            blasDesc.debugName = "BLAS";

            nvrhi::rt::AccelStructHandle blas;
            GetDevice()->createAccelStruct(blasDesc, &blas);
            nvrhi::utils::BuildBottomLevelAccelStruct(commandList, blas, blasDesc);
            m_BottomLevelAS.push_back(blas);
        }

        nvrhi::rt::AccelStructDesc tlasDesc;
        tlasDesc.isTopLevel = true;
        tlasDesc.topLevelMaxInstances = m_Instances.size();
        tlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::AllowUpdate;
        tlasDesc.debugName = "TLAS";
        GetDevice()->createAccelStruct(tlasDesc, &m_TopLevelAS);

        FillInstanceDescs();
        commandList->buildTopLevelAccelStruct(m_TopLevelAS, m_InstanceDescs.data(), m_InstanceDescs.size(), tlasDesc.buildFlags);
    }

    bool CreateRayTracingPipeline()
    {
        m_ShaderLibrary = m_ShaderFactory->CreateShaderLibrary("app/hello_dxr.hlsl", nullptr);
        if (!m_ShaderLibrary)
            return false;

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::All;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::RayTracingAccelStruct(0),
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
            nvrhi::BindingLayoutItem::Texture_UAV(0)
        };
        GetDevice()->createBindingLayout(layoutDesc, &m_BindingLayout);

        nvrhi::ShaderHandle rayGen, miss, shadowMiss, closestHit;
        m_ShaderLibrary->getShader("RayGen", nvrhi::ShaderType::RayGeneration, &rayGen);
        m_ShaderLibrary->getShader("Miss", nvrhi::ShaderType::Miss, &miss);
        m_ShaderLibrary->getShader("ShadowMiss", nvrhi::ShaderType::Miss, &shadowMiss);
        m_ShaderLibrary->getShader("ClosestHit", nvrhi::ShaderType::ClosestHit, &closestHit);

        nvrhi::rt::PipelineDesc pipelineDesc;
        pipelineDesc.globalBindingLayouts = { m_BindingLayout };
        pipelineDesc.shaders = {
            { "", rayGen, nullptr },
            { "", miss, nullptr },
            { "", shadowMiss, nullptr }
        };
        // The shadow hit group has no shaders: a hit leaves the payload's isHit = 1 untouched.
        pipelineDesc.hitGroups = {
            { "HitGroup", closestHit, nullptr, nullptr, nullptr, false },
            { "ShadowHitGroup", nullptr, nullptr, nullptr, nullptr, false }
        };
        pipelineDesc.maxPayloadSize = sizeof(float4);
        pipelineDesc.maxAttributeSize = sizeof(float2);
        pipelineDesc.maxRecursionDepth = 2; // primary ray + shadow ray
        GetDevice()->createRayTracingPipeline(pipelineDesc, &m_Pipeline);

        // Record order defines the hit group / miss indices the shader passes to TraceRay.
        m_Pipeline->createShaderTable(nvrhi::rt::ShaderTableDesc(), &m_ShaderTable);
        m_ShaderTable->setRayGenerationShader("RayGen");
        m_ShaderTable->addMissShader("Miss");         // miss index 0
        m_ShaderTable->addMissShader("ShadowMiss");   // miss index 1
        m_ShaderTable->addHitGroup("HitGroup");       // hit group 0
        m_ShaderTable->addHitGroup("ShadowHitGroup"); // hit group 1
        return true;
    }

    void AnimateInstances()
    {
        const float t = m_Time;
        m_Instances[0].transform = rotation(float3(0.f, 1.f, 0.f), -3.f * t) * translation(float3(0.f, 0.1f * cosf(0.5f * t), 0.f));
        m_Instances[1].transform = rotation(float3(0.f, 0.f, 1.f), 1.f * t) * translation(float3(0.f, 0.1f * cosf(0.7f * t), 0.f))
            * translation(float3(-4.f, 0.f, 0.f));
        m_Instances[2].transform = rotation(float3(1.f, 0.f, 0.f), 2.f * t) * translation(float3(0.f, 0.1f * cosf(1.2f * t), 0.f))
            * translation(float3(4.f, 0.f, 0.f));
    }

    void FillInstanceDescs()
    {
        for (size_t i = 0; i < m_Instances.size(); ++i)
        {
            const Instance& inst = m_Instances[i];
            nvrhi::rt::InstanceDesc& desc = m_InstanceDescs[i];
            desc.bottomLevelAS = m_BottomLevelAS[inst.mesh];
            desc.instanceID = uint32_t(i);
            desc.instanceMask = 0xFF;
            desc.instanceContributionToHitGroupIndex = 0;
            desc.flags = nvrhi::rt::InstanceFlags::None;
            affineToColumnMajor(inst.transform, desc.transform);

            InstanceData& data = m_InstanceData[i];
            // Row-vector convention: n_world = n_local * transpose(inverse(linear)).
            const float3x3 normalMatrix = transpose(inverse(inst.transform.m_linear));
            data.normalMatrix = affineToHomogeneous(affine3(normalMatrix, float3(0.f)));
            data.vertexOffset = m_Geometry.meshes[inst.mesh].vertexOffset;
            data.indexOffset = m_Geometry.meshes[inst.mesh].indexOffset;
        }
    }

    bool KeyboardUpdate(int key, int scancode, int action, int mods) override
    {
        m_Camera.KeyboardUpdate(key, scancode, action, mods);
        return true;
    }

    bool MousePosUpdate(double xpos, double ypos) override
    {
        m_Camera.MousePosUpdate(xpos, ypos);
        return true;
    }

    bool MouseButtonUpdate(int button, int action, int mods) override
    {
        m_Camera.MouseButtonUpdate(button, action, mods);
        return true;
    }

    bool MouseScrollUpdate(double xoffset, double yoffset) override
    {
        m_Camera.MouseScrollUpdate(xoffset, yoffset);
        return true;
    }

    void Animate(float elapsedTimeSeconds) override
    {
        m_Time += elapsedTimeSeconds;
        m_LightAngle += elapsedTimeSeconds;
        m_Camera.Animate(elapsedTimeSeconds);
        AnimateInstances();
        GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle);
    }

    void BackBufferResizing() override
    {
        m_Output = nullptr;
        m_BindingSet = nullptr;
        m_BindingCache->Clear();
    }

    void Render(nvrhi::IFramebuffer* framebuffer) override
    {
        const auto& fbinfo = framebuffer->getFramebufferInfo();

        if (!m_Output)
        {
            nvrhi::TextureDesc desc;
            desc.width = fbinfo.width;
            desc.height = fbinfo.height;
            desc.format = nvrhi::Format::RGBA16_FLOAT;
            desc.isUAV = true;
            desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.keepInitialState = true;
            desc.debugName = "RayTracingOutput";
            GetDevice()->createTexture(desc, &m_Output);

            nvrhi::BindingSetDesc setDesc;
            setDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstants),
                nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_TopLevelAS),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_InstanceBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_VertexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_IndexBuffer),
                nvrhi::BindingSetItem::Texture_UAV(0, m_Output)
            };
            GetDevice()->createBindingSet(setDesc, m_BindingLayout, &m_BindingSet);
        }

        // Camera: D3D-style projection (z in [0,1]); the shader unprojects through the inverse.
        const float aspect = float(fbinfo.width) / float(fbinfo.height);
        const float4x4 worldToClip = affineToHomogeneous(m_Camera.GetWorldToViewMatrix()) * perspProjD3DStyle(PI_f * 0.25f, aspect, 1.f, 1000.f);

        FrameConstants constants = {};
        constants.clipToWorld = inverse(worldToClip);
        constants.eyePosition = float4(m_Camera.GetPosition(), 1.f);
        const float3 lightPos = rotation(float3(0.f, 1.f, 0.f), m_LightAngle).transformPoint(float3(7.f, 6.f, 8.f));
        constants.lightPosition = float4(lightPos, 1.f);
        constants.lightStrength = float4(0.8f, 0.6f, 0.5f, 1.f);

        FillInstanceDescs();

        m_CommandList->open();

        m_CommandList->writeBuffer(m_InstanceBuffer, m_InstanceData.data(), m_InstanceData.size() * sizeof(InstanceData));
        m_CommandList->writeBuffer(m_FrameConstants, &constants, sizeof(constants));

        // Refit the TLAS with the new tetrahedron transforms.
        m_CommandList->buildTopLevelAccelStruct(m_TopLevelAS, m_InstanceDescs.data(), m_InstanceDescs.size(),
            nvrhi::rt::AccelStructBuildFlags::AllowUpdate | nvrhi::rt::AccelStructBuildFlags::PerformUpdate);

        nvrhi::rt::State state;
        state.shaderTable = m_ShaderTable;
        state.bindings = { m_BindingSet };
        m_CommandList->setRayTracingState(state);

        nvrhi::rt::DispatchRaysArguments args;
        args.width = fbinfo.width;
        args.height = fbinfo.height;
        m_CommandList->dispatchRays(args);

        m_CommonPasses->BlitTexture(m_CommandList, framebuffer, m_Output, m_BindingCache.get());

        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);
    }
};

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
    bool debug = false;
    for (int i = 1; i < __argc; ++i)
        if (strcmp(__argv[i], "--debug") == 0 || strcmp(__argv[i], "-debug") == 0)
            debug = true;
#ifdef _DEBUG
    debug = true;
#endif
    if (debug)
    {
        // Validation output of a windowed application: append it to hello_dxr.log next to the executable
        // (and to the debugger output), instead of Donut's message boxes.
        static FILE* logFile = nullptr;
#ifdef _WIN32
        fopen_s(&logFile, (app::GetDirectoryWithExecutable() / "hello_dxr.log").string().c_str(), "a");
#else
        logFile = fopen((app::GetDirectoryWithExecutable() / "hello_dxr.log").string().c_str(), "a");
#endif
        log::SetCallback([](log::Severity severity, const char* text) {
            if (logFile)
            {
                fprintf(logFile, "[%d] %s\n", int(severity), text);
                fflush(logFile);
            }
#ifdef _WIN32
            OutputDebugStringA(text);
            OutputDebugStringA("\n");
#endif
        });
        // An uncaught exception (e.g. a vulkan-hpp error at shutdown) ends in std::terminate: record it.
        std::set_terminate([]() {
            const char* what = "unknown";
            try { if (auto e = std::current_exception()) std::rethrow_exception(e); }
            catch (const std::exception& ex) { what = ex.what(); }
            catch (...) { what = "non-standard exception"; }
            if (logFile) { fprintf(logFile, "[terminate] %s\n", what); fflush(logFile); }
#ifdef _WIN32
            OutputDebugStringA(what);
#endif
            abort();
        });
    }

    auto deviceManager = nvrhi::TakeOver(app::DeviceManager::Create(api));

    app::DeviceCreationParameters deviceParams;
    deviceParams.backBufferWidth = 800;
    deviceParams.backBufferHeight = 600;
    deviceParams.enableRayTracingExtensions = true;
    deviceParams.enableDebugRuntime = debug;
    deviceParams.enableNvrhiValidationLayer = debug;

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, g_WindowTitle))
    {
        log::fatal("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }

    if (!deviceManager->GetDevice()->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline))
    {
        log::fatal("The graphics device does not support Ray Tracing Pipelines");
        return 1;
    }

    {
        HelloDXR example(deviceManager);
        if (example.Init())
        {
            deviceManager->AddRenderPassToBack(&example);
            deviceManager->RunMessageLoop();
            deviceManager->RemoveRenderPass(&example);
        }
    }

    deviceManager->Shutdown();
    return 0;
}
