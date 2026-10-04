#include "scene/SceneGraph.h"

using namespace donut::math;

SceneNode::SceneNode(std::shared_ptr<SceneNode> parent, std::string name, SceneMeshInstance* meshInstance)
    : m_Name(std::move(name))
    , m_MeshInstance(meshInstance)
    , m_Parent(std::move(parent))
{
    m_Children.reserve(10);
}

void SceneNode::AddChild(std::shared_ptr<SceneNode> child)
{
    m_Children.push_back(std::move(child));
}

std::shared_ptr<SceneNode> SceneNode::Find(const std::string& name, const std::string& parentName)
{
    if (m_Name == name && (parentName.empty() || (m_Parent && m_Parent->m_Name == parentName)))
        return shared_from_this();

    for (const auto& child : m_Children)
    {
        if (auto found = child->Find(name, parentName))
            return found;
    }
    return nullptr;
}

void SceneNode::UpdateTransforms(const affine3& parentWorld)
{
    // Row-vector convention: A * B applies A first.
    m_WorldTransform = m_Transform * m_BaseTransform * parentWorld;

    if (m_MeshInstance)
    {
        m_MeshInstance->previousTransform = m_MeshInstance->transform;
        m_MeshInstance->transform = m_WorldTransform;
    }

    for (const auto& child : m_Children)
        child->UpdateTransforms(m_WorldTransform);
}

void SceneNode::Print(std::ostream& os, uint32_t depth) const
{
    for (uint32_t i = 0; i < depth; ++i)
        os << "  ";

    os << m_Name << " " << m_Children.size() << " mesh "
       << (m_MeshInstance ? m_MeshInstance->name : std::string("null"))
       << " kids:" << std::endl;

    for (const auto& child : m_Children)
        child->Print(os, depth + 1);
}

std::ostream& operator<<(std::ostream& os, const std::shared_ptr<SceneNode>& root)
{
    os << "Graph ";
    if (root)
        root->Print(os, 0);
    return os;
}

box3 TransformBox(const box3& box, const affine3& transform)
{
    if (box.isempty())
        return box3::empty();

    box3 result = box3::empty();
    for (int corner = 0; corner < 8; ++corner)
        result |= transform.transformPoint(box.getCorner(corner));
    return result;
}
