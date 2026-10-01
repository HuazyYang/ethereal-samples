#ifndef SCENE_HPP
#define SCENE_HPP
#include <donut/core/object/Foundation.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/math/math.h>
#include <donut/core/object/AutoPtr.h>
#include "SampleTypes.h"

namespace SampleUtils {

class Camera;
class Light;

class Model : public donut::ObjectImpl<donut::IObject> {
 public:
    int loadObj(donut::vfs::IFileSystem *vfs, const char *filePath,
                const dm::float3 &pos = {0.f}, const dm::quat &rot = {},
                const dm::float3 &scaling = {1.f});

    void setPosition(const dm::float3 &pos);
    void setRotation(const dm::quat &rot);
    void setScaling(const dm::float3 &scaling);

    Range<const dm::float3> getPositionBuffer();
    Range<const dm::float3> getNormalBuffer();
    Range<const dm::float2> getUVBuffer();
    Range<const dm::int3> getIndexBuffer();
    const dm::affine3 &getLocalMatrix();
    const dm::box3 &getLocalBounds();

 private:
    void updateWorldTransform();
    // Triangle mesh
    std::vector<dm::float3> m_positions;
    std::vector<dm::float3> m_normals;
    std::vector<dm::float2> m_uvs;
    std::vector<dm::int3> m_indices;
    dm::box3 m_localBounds;

    dm::float3 m_translation;
    dm::quat m_rotation;
    dm::float3 m_scaling;
    dm::affine3 m_matLocalToWorld;
};

class Scene : public donut::ObjectImpl<donut::IObject> {
 public:
    Scene();
    ~Scene();

    int addModel(Model *model);
    Model *getModel(int idx);

    void setCamera(Camera *camera);
    Camera *getCamera();

    int addLight(Light *light);
    Light *getLight(int idx);

 private:
    std::vector<donut::AutoPtr<Model>> m_models;
    donut::AutoPtr<Camera> m_camera;
    std::vector<donut::AutoPtr<Light>> m_lights;
};

}  // namespace SampleUtils

#endif /* SCENE_HPP */
