#pragma once

// 2018 framework scene-graph types used by SpaceObject (ported from the pre-2019 donut Scene).
//
// deviation: donut main has donut::engine::SceneGraph / MeshInfo / MeshInstance with a different
// ownership and transform model (leaves, dirty flags, double-precision transforms). The 2018 demo
// drives its ship through this simpler graph (node transform x base transform x parent world, pushed
// into the attached mesh instance), so it is reconstructed as-is under Scene* names to avoid
// clashing with donut's types.

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

struct SceneMaterial;

// Vertex attribute bits (2018 donut VertexAttribute), used as BufferGroup keys and in the attribute mask.
enum SceneVertexAttribute : uint32_t
{
    VertexAttr_Position     = 0x01,
    VertexAttr_Texcoord1    = 0x02,
    VertexAttr_Texcoord2    = 0x04,
    VertexAttr_Normal       = 0x08,
    VertexAttr_Tangent      = 0x10,
    VertexAttr_Bitangent    = 0x20,
    VertexAttr_Transform    = 0x40,
    VertexAttr_PrevTransform= 0x80,
    VertexAttr_All          = 0xFF,
};

// Index buffer plus one vertex buffer per attribute bit (SpaceObject+136).
struct SceneBufferGroup
{
    nvrhi::BufferHandle indexBuffer;
    std::map<int, nvrhi::BufferHandle> vertexBuffers;

    nvrhi::IBuffer* GetVertexBuffer(int attribute) const
    {
        auto it = vertexBuffers.find(attribute);
        return it != vertexBuffers.end() ? it->second.Get() : nullptr;
    }
};

// 2018 MeshInfo (0x58 bytes)
struct SceneMesh
{
    std::string name;
    SceneMaterial* material = nullptr;
    SceneBufferGroup* buffers = nullptr;
    donut::math::box3 objectSpaceBounds = donut::math::box3::empty();
    uint32_t indexOffset = 0;
    uint32_t vertexOffset = 0;
    uint32_t numIndices = 0;
    uint32_t numVertices = 0;
};

// 2018 MeshInstance (0xB8 bytes)
struct SceneMeshInstance
{
    std::string name;
    SceneMesh* mesh = nullptr;
    donut::math::affine3 transform = donut::math::affine3::identity();
    donut::math::affine3 previousTransform = donut::math::affine3::identity();
    donut::math::box3 transformedBounds = donut::math::box3::empty();
    donut::math::float3 transformedCenter = 0.f;
    uint32_t instanceIndex = 0;     // row in the "Transforms" buffer
    uint32_t firstMeshlet = 0;
    uint32_t numMeshlets = 0;
};

// Asteroids.exe: scene-graph node (0xF0 bytes; ctor 0x14004B6F0, Find 0x14004BD50,
// UpdateTransforms 0x14004C170, print 0x14004BF80 / 0x14004BC20)
class SceneNode : public std::enable_shared_from_this<SceneNode>
{
public:
    SceneNode(std::shared_ptr<SceneNode> parent, std::string name, SceneMeshInstance* meshInstance = nullptr);

    const std::string& GetName() const { return m_Name; }
    const std::shared_ptr<SceneNode>& GetParent() const { return m_Parent; }
    const std::vector<std::shared_ptr<SceneNode>>& GetChildren() const { return m_Children; }

    // User / animation transform (applied before the file transform).
    const donut::math::affine3& GetTransform() const { return m_Transform; }
    void SetTransform(const donut::math::affine3& transform) { m_Transform = transform; }

    // Transform from the source file (aiNode::mTransformation).
    const donut::math::affine3& GetBaseTransform() const { return m_BaseTransform; }
    void SetBaseTransform(const donut::math::affine3& transform) { m_BaseTransform = transform; }

    const donut::math::affine3& GetWorldTransform() const { return m_WorldTransform; }

    SceneMeshInstance* GetMeshInstance() const { return m_MeshInstance; }
    void SetMeshInstance(SceneMeshInstance* instance) { m_MeshInstance = instance; }

    // Does not set the child's parent (the parent is given to the child's constructor).
    void AddChild(std::shared_ptr<SceneNode> child);

    // Depth-first search; 'parentName' (if non-empty) must match the parent's name.
    std::shared_ptr<SceneNode> Find(const std::string& name, const std::string& parentName = std::string());

    // world = transform * baseTransform * parentWorld; pushes the result into the mesh instance
    // (saving the old one as previousTransform) and recurses into the children.
    void UpdateTransforms(const donut::math::affine3& parentWorld = donut::math::affine3::identity());

    void Print(std::ostream& os, uint32_t depth) const;

private:
    std::string m_Name;
    donut::math::affine3 m_Transform = donut::math::affine3::identity();
    SceneMeshInstance* m_MeshInstance = nullptr;
    donut::math::affine3 m_BaseTransform = donut::math::affine3::identity();
    donut::math::affine3 m_WorldTransform = donut::math::affine3::identity();
    std::shared_ptr<SceneNode> m_Parent;
    std::vector<std::shared_ptr<SceneNode>> m_Children;
};

// Prints "Graph " followed by one line per node: indentation, name, child count, " mesh <name|null> kids:".
std::ostream& operator<<(std::ostream& os, const std::shared_ptr<SceneNode>& root);

// Transforms the 8 corners of 'box' and returns their bounds (2018 0x140051A30).
donut::math::box3 TransformBox(const donut::math::box3& box, const donut::math::affine3& transform);
