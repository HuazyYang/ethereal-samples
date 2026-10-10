// GVDB volumes in a Donut scene graph (see GVDBScene.h).
#include <gvdb/GVDBScene.h>
#include <donut/core/log.h>
#include <algorithm>
#include <cmath>

namespace gvdb {

using namespace donut;

// ---- VolumeTransferFunction -------------------------------------------------

VolumeTransferFunction::VolumeTransferFunction(gp::IDevice *device)
    : m_data(size_t(TRANSFER_FUNC_SIZE), dm::float4::zero()), m_device(device) {}

void VolumeTransferFunction::setLinear(float t0, float t1, dm::float4 a, dm::float4 b) {
    const int n = int(m_data.size());
    int i0 = int(std::floor(t0 * n));
    int i1 = int(std::floor(t1 * n));
    i0 = std::clamp(i0, 0, n);
    i1 = std::clamp(i1, 0, n);
    if (i1 <= i0) return;
    const float span = float(i1 - i0);
    for (int i = i0; i < i1; ++i) {
        const float u = float(i - i0) / span;
        m_data[i] = a + (b - a) * u;
    }
}

void VolumeTransferFunction::commit(gp::IDeviceQueue *queue) {
    if (!queue || !m_device) return;
    const uint64_t bytes = uint64_t(m_data.size()) * sizeof(dm::float4);
    if (!m_gpu) {
        gp::BufferDesc desc;
        desc.byteSize = bytes;
        if (NVRHI_FAILED(m_device->createBuffer(desc, &m_gpu))) {
            log::error("GVDB: cannot create the transfer function buffer");
            return;
        }
    }
    if (NVRHI_FAILED(queue->writeBuffer(m_gpu, m_data.data(), bytes, 0)))
        log::error("GVDB: cannot upload the transfer function");
}

// ---- GVDBVolumeInstance -----------------------------------------------------

dm::box3 GVDBVolumeInstance::GetLocalBoundingBox() {
    if (!m_volume) return dm::box3::empty();
    dm::box<int, 3> b = m_volume->getVoxelBounds();
    if (b.isempty()) return dm::box3::empty();
    return dm::box3(dm::float3(b.lower()), dm::float3(b.upper()));
}

nvrhi::AutoPtr<engine::SceneGraphLeaf> GVDBVolumeInstance::Clone() {
    auto copy = MAKE_RC_OBJ_PTR(GVDBVolumeInstance, m_volume.Get());   // shares the volume
    copy->m_attributes = m_attributes;
    return copy;
}

dm::affine3 GVDBVolumeInstance::GetIndexToWorld() const {
    engine::SceneGraphNode *node = GetNode();
    if (!node) return dm::affine3::identity();
    return node->GetLocalToWorldTransformFloat();
}

dm::affine3 GVDBVolumeInstance::GetWorldToIndex() const { return dm::inverse(GetIndexToWorld()); }

// ---- GVDBSceneGraph ---------------------------------------------------------

void GVDBSceneGraph::RegisterLeaf(engine::SceneGraphLeaf *leaf) {
    SceneGraph::RegisterLeaf(leaf);
    if (!leaf) return;

    auto instance = donut::query_cast<GVDBVolumeInstance>(leaf);
    if (!instance) return;

    IGVDBVolume *volume = instance->GetVolume();
    if (volume && m_volumes.AddRef(volume) && OnVolumeAdded) OnVolumeAdded(volume);
    m_volumeInstances.push_back(instance);
}

void GVDBSceneGraph::UnregisterLeaf(engine::SceneGraphLeaf *leaf) {
    SceneGraph::UnregisterLeaf(leaf);
    if (!leaf) return;

    auto instance = donut::query_cast<GVDBVolumeInstance>(leaf);
    if (!instance) return;

    IGVDBVolume *volume = instance->GetVolume();
    if (volume && m_volumes.Release(volume) && OnVolumeRemoved) OnVolumeRemoved(volume);

    auto it = std::find(m_volumeInstances.begin(), m_volumeInstances.end(), instance);
    if (it != m_volumeInstances.end()) m_volumeInstances.erase(it);
}

}  // namespace gvdb
