#include "scene/SpaceSector.h"
#include "scene/SceneGraph.h"

#include <algorithm>

using namespace donut::math;

SpaceSector::SpaceSector(uint32_t numAsteroidTypes)
    : m_Instances(numAsteroidTypes)
    , m_InstanceBuffers(numAsteroidTypes)
    , m_BindingSets(numAsteroidTypes)
    , m_MaxRadius(numAsteroidTypes, 0.f)
{
}

void SpaceSector::Clear()
{
    // Asteroids.exe: 0x140060DB0
    for (auto& instances : m_Instances)
        instances.clear();
    m_InstancesUploaded = false;
}

void SpaceSector::AddInstance(uint32_t typeId, const float3& position, const float3& scaling, const float3& rotation)
{
    // Asteroids.exe: 0x140060FF0
    if (typeId >= m_Instances.size())
        return;

    AsteroidPlacement placement;
    placement.typeId = typeId;
    placement.position = position;
    placement.rotation = rotation;
    placement.scaling = scaling;
    m_Instances[typeId].push_back(placement);
}

void SpaceSector::UpdateTransformsAndBounds(const std::function<box3(uint32_t)>& getTypeBounds)
{
    // Asteroids.exe: 0x1400616F0
    m_Bounds = box3::empty();
    for (size_t t = 0; t < m_Instances.size(); ++t)
    {
        if (m_Instances[t].empty())
            continue;

        m_MaxRadius[t] = 0.f;
        for (AsteroidPlacement& p : m_Instances[t])
        {
            const affine3 transform = affine3::identity()
                * yawPitchRoll(p.rotation.x, p.rotation.y, p.rotation.z)
                * scaling(p.scaling)
                * translation(p.position);
            p.transform = transform;
            p.inverseTransform = inverse(transform);
            p.transformValid = true;

            p.bounds = TransformBox(getTypeBounds(p.typeId), transform);
            p.sphereCenter = (p.bounds.m_mins + p.bounds.m_maxs) * 0.5f;
            p.sphereRadius = length(p.bounds.m_maxs - p.bounds.m_mins) * 0.5f;

            m_MaxRadius[t] = std::max(m_MaxRadius[t], p.sphereRadius);
            m_Bounds |= p.bounds;
        }
    }
}

void SpaceSector::CreateInstanceBuffers(nvrhi::IDevice* device)
{
    // Asteroids.exe: 0x140060E80
    for (size_t t = 0; t < m_Instances.size(); ++t)
    {
        if (!m_Instances[t].empty())
        {
            // deviation: explicit structured-buffer stride for donut's nvrhi (the 2018 desc had stride 0).
            nvrhi::BufferDesc desc;
            desc.byteSize = m_Instances[t].size() * sizeof(AsteroidInstance);
            desc.structStride = sizeof(AsteroidInstance);
            desc.debugName = "Instances";
            desc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            device->createBuffer(desc, &m_InstanceBuffers[t]);
        }
        else
            m_InstanceBuffers[t] = nullptr;

        m_BindingSets[t] = nullptr;
    }
    m_InstancesUploaded = false;
}

void SpaceSector::Update(nvrhi::ICommandList* commandList)
{
    // Asteroids.exe: 0x140061410
    if (m_InstancesUploaded)
        return;

    std::vector<AsteroidInstance> data;
    for (size_t t = 0; t < m_Instances.size(); ++t)
    {
        const auto& instances = m_Instances[t];
        if (instances.empty())
            continue;

        data.resize(instances.size());
        for (size_t i = 0; i < instances.size(); ++i)
        {
            const AsteroidPlacement& p = instances[i];
            AsteroidInstance& gpu = data[i];
            affineToColumnMajor(p.transform, gpu.instanceMat);
            gpu.uniformScale = std::max(p.scaling.x, std::max(p.scaling.y, p.scaling.z));
            gpu.padding[0] = gpu.padding[1] = gpu.padding[2] = 0.f;
        }

        if (m_InstanceBuffers[t])
            commandList->writeBuffer(m_InstanceBuffers[t], data.data(), instances.size() * sizeof(AsteroidInstance));
    }
    m_InstancesUploaded = true;
}

uint32_t SpaceSector::GetTotalInstanceCount() const
{
    // Asteroids.exe: 0x1400613C0
    uint32_t total = 0;
    for (const auto& instances : m_Instances)
        total += uint32_t(instances.size());
    return total;
}
