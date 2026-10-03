#include "SampleTypes.h"

namespace SampleUtils {

using namespace donut;

dm::affine3 makeVolumeTransform(dm::float3 pretrans, dm::float3 scale, dm::float3 anglesDeg, dm::float3 trans) {
    dm::affine3 rot = dm::rotationQuat(dm::radians(anglesDeg)).toAffine();
    return dm::translation(pretrans) * dm::scaling(scale) * rot * dm::translation(trans);
}

dm::affine3 vbxTransformToAffine(const gvdb::VBXTransform &xf) {
    return makeVolumeTransform(xf.pretrans, xf.scale, xf.angles, xf.trans);
}

void setNodeTransform(engine::SceneGraphNode *node, const dm::affine3 &transform) {
    if (!node) return;
    dm::float3 t, s;
    dm::quat q;
    dm::decomposeAffine(transform, &t, &q, &s);
    dm::double3 dt(t), ds(s);
    dm::dquat dq(q);
    node->SetTransform(&dt, &dq, &ds);
}

nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> attachVolumeInstance(gvdb::GVDBSceneGraph *graph, gvdb::IGVDBVolume *volume,
                                                             const dm::affine3 &indexToWorld, const std::string &name) {
    if (!graph || !volume || !graph->GetRootNode()) return nullptr;
    auto instance = MAKE_RC_OBJ_PTR(gvdb::GVDBVolumeInstance, volume);
    auto node = graph->AttachLeafNode(graph->GetRootNode(), instance);
    node->SetName(name);
    setNodeTransform(node, indexToWorld);
    graph->Refresh(0);
    return instance;
}

}  // namespace SampleUtils
