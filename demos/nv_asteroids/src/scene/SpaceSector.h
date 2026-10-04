#pragma once

// One cell of the asteroid field grid: per asteroid type, the placed instances (sector-local
// transforms) and the "Instances" GPU buffer consumed by asteroidTS/MS.
//
// Asteroids.exe: SpaceSector (0x88 bytes, make_shared 0x1400611E0; AddInstance 0x140060FF0,
// UpdateTransformsAndBounds 0x1400616F0, CreateInstanceBuffers 0x140060E80, Update 0x140061410,
// Clear 0x140060DB0). AsteroidPlacement: 0xCC-byte CPU record.

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <functional>
#include <vector>

// StructuredBuffer<AsteroidInstance> instanceInfoBuffer (t11, space1)
struct AsteroidInstance
{
    float instanceMat[12];      // row_major float3x4 = column-major affine (row r = (L[0][r], L[1][r], L[2][r], T[r]))
    float uniformScale;         // max(scale.x, scale.y, scale.z)
    float padding[3];
};
static_assert(sizeof(AsteroidInstance) == 64);

// cbuffer cbSectorInfo (b5, space1); filled per draw by the meshlet draw strategy.
struct SectorInfo
{
    donut::math::float3 sectorOffset;   // SpaceScene::GetSectorOrigin(cell) + camera rebasing offset
    uint32_t asteroidId;
};
static_assert(sizeof(SectorInfo) == 16);

struct AsteroidPlacement
{
    uint32_t typeId = 0;
    donut::math::float3 position = 0.f;     // sector-local
    donut::math::float3 rotation = 0.f;     // yaw, pitch, roll in radians (raw from asteroid-placement.json)
    donut::math::float3 scaling = 1.f;
    donut::math::affine3 transform = donut::math::affine3::identity();         // sector-local
    donut::math::affine3 inverseTransform = donut::math::affine3::identity();
    bool transformValid = false;
    donut::math::box3 bounds = donut::math::box3::empty();                      // sector-local
    donut::math::float3 sphereCenter = 0.f;
    float sphereRadius = 0.f;
};

class SpaceSector
{
public:
    explicit SpaceSector(uint32_t numAsteroidTypes);

    void Clear();
    void AddInstance(uint32_t typeId, const donut::math::float3& position, const donut::math::float3& scaling,
        const donut::math::float3& rotation);

    // transform = yawPitchRoll(rotation) * scaling * translation(position); bounds from the type's bounds.
    void UpdateTransformsAndBounds(const std::function<donut::math::box3(uint32_t typeId)>& getTypeBounds);

    // One "Instances" buffer per type with instances (null otherwise); resets the binding-set cache.
    void CreateInstanceBuffers(nvrhi::IDevice* device);
    // Uploads the instance data once after CreateInstanceBuffers / a placement reload.
    void Update(nvrhi::ICommandList* commandList);

    uint32_t GetNumTypes() const { return uint32_t(m_Instances.size()); }
    const std::vector<AsteroidPlacement>& GetInstances(uint32_t typeId) const { return m_Instances[typeId]; }
    const AsteroidPlacement& GetInstance(uint32_t typeId, uint32_t index) const { return m_Instances[typeId][index]; }
    uint32_t GetInstanceCount(uint32_t typeId) const { return uint32_t(m_Instances[typeId].size()); }
    uint32_t GetTotalInstanceCount() const;
    nvrhi::IBuffer* GetInstanceBuffer(uint32_t typeId) const { return m_InstanceBuffers[typeId]; }
    // Binding-set cache slot owned by the meshlet draw strategy (instance buffer at t11).
    nvrhi::BindingSetHandle& GetBindingSet(uint32_t typeId) { return m_BindingSets[typeId]; }
    // Largest bounding-sphere radius of the type's instances (screen-size culling).
    float GetMaxRadius(uint32_t typeId) const { return m_MaxRadius[typeId]; }
    const donut::math::box3& GetBounds() const { return m_Bounds; }    // sector-local
    bool AreInstancesUploaded() const { return m_InstancesUploaded; }

private:
    std::vector<std::vector<AsteroidPlacement>> m_Instances;
    std::vector<nvrhi::BufferHandle> m_InstanceBuffers;
    std::vector<nvrhi::BindingSetHandle> m_BindingSets;
    std::vector<float> m_MaxRadius;
    donut::math::box3 m_Bounds = donut::math::box3::empty();
    bool m_InstancesUploaded = false;
};
