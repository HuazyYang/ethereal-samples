#include "Scene.h"
#include "ObjLoader.h"
#include "Camera.h"

namespace SampleUtils {

int Model::loadObj(donut::vfs::IFileSystem* vfs, const char* filePath,
                       const dm::float3& pos, const dm::quat& rot,
                       const dm::float3& scaling) {
    auto mesh = ObjLoader()(vfs, filePath);
    if(dm::any(pos != dm::float3::zero()) || rot.w != 1.f || dm::any(scaling != dm::float3{1.f})) {
        auto ptrTransform =
            dm::scaling(scaling) * rot.toAffine() * dm::translation(pos);
        auto normalTransform = dm::conjugate(rot).toAffine();

        size_t n = mesh.positions.size();
        for (size_t i = 0; i < n; ++i) {
            auto& pos = mesh.positions[i];
            pos = ptrTransform.transformPoint(pos);
            auto& normal = mesh.normals[i];
            normal = normalTransform.transformPoint(normal);
        }

        mesh.bounds = dm::box3::empty();
        for (size_t i = 0; i < mesh.indices.size(); ++i) {
            const auto &triIdx = mesh.indices[i];
            mesh.bounds |= mesh.positions[triIdx.x];
            mesh.bounds |= mesh.positions[triIdx.y];
            mesh.bounds |= mesh.positions[triIdx.z];
        }
    }

    m_positions = std::move(mesh.positions);
    m_normals = std::move(mesh.normals);
    m_uvs.clear();
    m_indices = std::move(mesh.indices);
    m_localBounds = mesh.bounds;

    return 0;
}

void Model::setPosition(const dm::float3& pos) {
    m_translation = pos;
    updateWorldTransform();
}

void Model::setRotation(const dm::quat& rot) {
    m_rotation = rot;
    updateWorldTransform();
}

void Model::setScaling(const dm::float3& scaling) {
    m_scaling = scaling;
    updateWorldTransform();
}

Range<const dm::float3> Model::getPositionBuffer() {
    return Range<const dm::float3>{m_positions.data(),
                                   m_positions.data() + m_positions.size()};
}

Range<const dm::float3> Model::getNormalBuffer() {
    return Range<const dm::float3>{m_normals.data(), m_normals.data() + m_normals.size()};
}

Range<const dm::float2> Model::getUVBuffer() { return Range<const dm::float2>{m_uvs.data(), m_uvs.data() + m_uvs.size()}; }

Range<const dm::int3> Model::getIndexBuffer() {
    return Range<const dm::int3>{m_indices.data(), m_indices.data() + m_indices.size()};
}

const dm::affine3& Model::getLocalMatrix() { return m_matLocalToWorld; }

const dm::box3& Model::getLocalBounds() { return m_localBounds; }

void Model::updateWorldTransform() {
    m_matLocalToWorld =
        dm::scaling(m_scaling) * m_rotation.toAffine() * dm::translation(m_translation);
}

Scene::Scene() {}

Scene::~Scene() {}

int Scene::addModel(Model* model) {
    if (!model) return -1;

    m_models.push_back(model);
    return static_cast<int>(m_models.size() - 1);
}

Model* Scene::getModel(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_models.size())) return nullptr;

    return m_models[idx];
}

void Scene::setCamera(Camera* camera) { m_camera = camera; }

Camera* Scene::getCamera() { return m_camera; }

int Scene::addLight(Light* light) {
    if (!light) return -1;

    m_lights.push_back(light);
    return static_cast<int>(m_lights.size() - 1);
}

Light* Scene::getLight(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_lights.size())) return nullptr;
    return m_lights[idx];
}

}  // namespace gvdb