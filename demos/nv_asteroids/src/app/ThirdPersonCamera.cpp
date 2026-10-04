#include "app/ThirdPersonCamera.h"
#include "scene/CargoShip.h"
#include "scene/PlayerShip.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

using namespace donut::math;

namespace
{
    // The ship transform is the cargo ship's root node transform (2018: ship+0 -> 0x1400490B0).
    affine3 GetShipTransform(const PlayerShip* ship)
    {
        if (!ship || !ship->GetCargoShip())
            return affine3::identity();
        return ship->GetCargoShip()->GetTransform();
    }
}

ThirdPersonCamera::ThirdPersonCamera(PlayerShip* target, float distance, float yaw, float pitch)
    : m_Target(target)
    , m_Distance(distance)
    , m_Yaw(yaw)
    , m_Pitch(pitch)
    , m_DefaultYaw(yaw)
    , m_DefaultPitch(pitch)
{
    ResetRotation();
}

void ThirdPersonCamera::ResetRotation()
{
    m_Rotation = rotationQuat(float3(m_DefaultPitch, m_DefaultYaw, 0.f));
    m_Yaw = m_DefaultYaw;
    m_Pitch = m_DefaultPitch;
}

void ThirdPersonCamera::SetRotation(float yaw, float pitch)
{
    m_Yaw = yaw;
    m_Pitch = pitch;
    m_Rotation = rotationQuat(float3(pitch, yaw, 0.f));
}

void ThirdPersonCamera::MousePosUpdate(double xpos, double ypos)
{
    m_MousePosPrev = m_MousePos;
    m_MousePos = float2(float(xpos), float(ypos));
}

void ThirdPersonCamera::MouseButtonUpdate(int button, int action, int mods)
{
    // Mouse button map (+296): left -> 0 (orbit), middle -> 1, right -> 2.
    int index;
    switch (button)
    {
    case GLFW_MOUSE_BUTTON_LEFT: index = 0; break;
    case GLFW_MOUSE_BUTTON_MIDDLE: index = 1; break;
    case GLFW_MOUSE_BUTTON_RIGHT: index = 2; break;
    default: return;
    }

    const bool pressed = (action == GLFW_PRESS);
    if (pressed)
    {
        if (index == 0)
        {
            if (!m_MouseButtons[0])
            {
                m_LeftDragStart = m_MousePos;
                m_MouseButtons[0] = true;
                return;
            }
        }
        else if (index == 2 && !m_MouseButtons[2])
        {
            m_RightDragStart = m_MousePos;
        }
    }

    m_MouseButtons[index] = pressed;
}

void ThirdPersonCamera::MouseScrollUpdate(double xoffset, double yoffset)
{
    const float scale = (yoffset >= 0.0) ? (1.f / 1.15f) : 1.15f;
    m_Distance = std::min(std::max(c_MinDistance, scale * m_Distance), c_MaxDistance);
}

void ThirdPersonCamera::JoystickButtonUpdate(int button, bool pressed)
{
    // Gamepad A/B: camera distance.
    if (button == 0)
    {
        if (pressed)
            m_JoystickDistance += 1.f;
    }
    else if (button == 1)
    {
        if (pressed)
            m_JoystickDistance -= 1.f;
    }
}

void ThirdPersonCamera::JoystickUpdate(int axis, float value)
{
    // Right stick: camera rotation.
    if (axis == 2)
        m_JoystickYaw = value;
    else if (axis == 3)
        m_JoystickPitch = value;
}

void ThirdPersonCamera::UpdateHistory(float deltaT, const float3& frameOffset)
{
    const affine3 shipTransform = GetShipTransform(m_Target);

    if (m_History.empty())
    {
        // The first entry starts "old" so that the lag interpolation has a predecessor.
        HistoryEntry entry;
        entry.transform = shipTransform;
        entry.age = m_LagTime + m_LagTime;
        m_History.push_back(entry);
    }

    auto oldestToKeep = m_History.end();
    for (auto it = m_History.begin(); it != m_History.end(); ++it)
    {
        it->transform.m_translation += frameOffset;
        it->age += deltaT;
        if (it->age > m_LagTime)
            oldestToKeep = it;
    }

    if (oldestToKeep != m_History.end())
        m_History.erase(m_History.begin(), oldestToKeep);

    HistoryEntry entry;
    entry.transform = shipTransform;
    entry.age = 0.f;
    m_History.push_back(entry);
}

void ThirdPersonCamera::Animate(float deltaT)
{
    float yaw = m_Yaw;
    float pitch = m_Pitch;

    if (m_MouseButtons[0])
    {
        const float2 delta = m_MousePos - m_MousePosPrev;
        m_MousePosPrev = m_MousePos;
        yaw -= delta.x * c_MouseRotateSpeed;
        pitch += delta.y * c_MouseRotateSpeed;
    }

    pitch += deltaT * 1.5f * m_JoystickPitch;
    yaw += deltaT * 1.5f * m_JoystickYaw;
    m_Yaw = yaw;
    m_Distance = std::min(std::max(c_MinDistance, deltaT * 40.f * m_JoystickDistance + m_Distance), c_MaxDistance);
    m_Pitch = std::max(std::min(pitch, c_MaxPitch), -c_MaxPitch);

    m_Rotation = rotationQuat(float3(m_Pitch, m_Yaw, 0.f));

    // The gamepad values are per-frame impulses.
    m_JoystickYaw = 0.f;
    m_JoystickPitch = 0.f;
    m_JoystickDistance = 0.f;

    // ship+108: the play-volume wrap correction of the frame, applied to the stored positions as well.
    UpdateHistory(deltaT, m_Target ? m_Target->GetBoundaryCorrection() : float3(0.f));

    // Ship position m_LagTime seconds ago, interpolated between the bracketing history entries.
    float3 laggedPosition = m_History.front().transform.m_translation;
    {
        const HistoryEntry* previous = &m_History.front();
        for (const HistoryEntry& entry : m_History)
        {
            if (m_LagTime >= entry.age)
            {
                const float range = previous->age - entry.age;
                const float t = (range != 0.f) ? (m_LagTime - entry.age) / range : 0.f;
                laggedPosition = lerp(entry.transform.m_translation, previous->transform.m_translation, t);
                break;
            }
            previous = &entry;
            laggedPosition = entry.transform.m_translation;
        }
    }

    const float3 shipPosition = GetShipTransform(m_Target).m_translation;

    float3 lagOffset = (laggedPosition - shipPosition) * 0.1f;
    const float lagLength = length(lagOffset);
    if (lagLength > c_MaxLagOffset)
        lagOffset = (lagOffset / lagLength) * c_MaxLagOffset;

    // Orbit offset in ship space: camera rotation composed with the ship orientation.
    const quat shipOrientation = m_Target ? m_Target->GetOrientation() : quat::identity();
    const quat orientation = normalize(shipOrientation) * normalize(m_Rotation);
    const float3 orbitOffset = applyQuat(orientation, float3(0.f, 0.f, -m_Distance));

    // The lag is applied fully when it points along the orbit direction and fades to 10% otherwise.
    float alignment = 0.1f;
    const float lagOffsetLength = length(lagOffset);
    const float orbitOffsetLength = length(orbitOffset);
    if (lagOffsetLength > 0.f && orbitOffsetLength > 0.f)
        alignment = std::max(dot(lagOffset / lagOffsetLength, orbitOffset / orbitOffsetLength), 0.1f);
    const float t = std::min(std::max((alignment - 0.1f) / 0.9f, 0.f), 1.f);
    const float lagWeight = (3.f - (t + t)) * (t * t) * 0.9f + 0.1f;

    const float3 cameraPosition = shipPosition + orbitOffset + lagOffset * lagWeight;

    // Smooth the up vector towards the ship's.
    const float3 shipUp = m_Target ? m_Target->GetUp() : float3(0.f, 1.f, 0.f);
    const float upBlend = std::min(std::max(deltaT + deltaT, 0.f), 1.f);
    m_Up = lerp(m_Up, shipUp, upBlend);
    if (length(m_Up) < 0.01f)
        m_Up = shipUp;

    BaseLookAt(cameraPosition, shipPosition, m_Up);
}
