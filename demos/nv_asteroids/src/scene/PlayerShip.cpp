#include "scene/PlayerShip.h"
#include "scene/CargoShip.h"

#include "audio/SoundEngine.h"

#include <donut/core/json.h>
#include <donut/core/vfs/VFS.h>

#include <json/json.h>

#include <algorithm>
#include <cmath>

using namespace donut;
using namespace donut::math;

PlayerShip::PlayerShip(CargoShip* cargoShip)
    : m_CargoShip(cargoShip)
{
}

PlayerShip::~PlayerShip() = default;

affine3 PlayerShip::GetModelOffset()
{
    // Dynamic initializer 0x140005830
    return translation(float3(0.f, 5.f, 0.f));
}

void PlayerShip::LoadControls(const Json::Value& controls)
{
    // Asteroids.exe: 0x140045CA0
    auto readClamped = [&controls](const char* key, float& value, float minimum)
    {
        const Json::Value& node = controls[key];
        if (node.isNumeric())
            value = std::max(node.asFloat(), minimum);
    };
    readClamped("top_speed_forward", m_TopSpeedForward, 1.f);
    readClamped("top_speed_reverse", m_TopSpeedReverse, 0.f);
    readClamped("acceleration_scale", m_AccelerationScale, 1.f);
    readClamped("rotation_sensitivity", m_RotationSensitivity, 0.01f);
    readClamped("roll_sensitivity", m_RollSensitivity, 0.01f);
    readClamped("time_to_stop", m_TimeToStop, 0.01f);
    m_PlayVolumeMin = json::Read<float3>(controls["play_volume_min"], m_PlayVolumeMin);
    m_PlayVolumeMax = json::Read<float3>(controls["play_volume_max"], m_PlayVolumeMax);
}

void PlayerShip::LoadSounds(const Json::Value& playerShipArray, nvrhi::AutoPtr<vfs::IFileSystem> fs,
    const std::filesystem::path& mediaPath)
{
    // Asteroids.exe: 0x140045EF0
    const Json::Value& ship = playerShipArray[0];

    if (ship["jetSound"].isString())
    {
        m_JetSound = audio::Sound::Create(fs, mediaPath / std::filesystem::path(ship["jetSound"].asString()), true);
        if (m_JetSound && ship["jetSoundVolume"].isNumeric())
            m_JetSound->SetVolume(ship["jetSoundVolume"].asFloat());
    }

    if (ship["shieldSound"].isString())
    {
        m_ShieldSound = audio::Sound::Create(fs, mediaPath / std::filesystem::path(ship["shieldSound"].asString()), false);
        if (m_ShieldSound && ship["shieldSoundVolume"].isNumeric())
            m_ShieldSound->SetVolume(ship["shieldSoundVolume"].asFloat());
    }
}

void PlayerShip::PlayShieldSound()
{
    // Asteroids.exe: 0x140046370. deviation: null check (the 2018 code assumes the sound loaded).
    if (!m_ShieldSound)
        return;

    if (!m_ShieldSound->IsPlaying())
    {
        m_ShieldSound->Rewind();
        m_ShieldSound->Play();
        m_ShieldSound->ExitLoop();
    }
}

void PlayerShip::UpdateBasis()
{
    // Asteroids.exe: 0x1400463E0
    const affine3 rotation = m_Orientation.toAffine();
    m_Forward = rotation.m_linear.row2;
    m_Up = rotation.m_linear.row1;
}

affine3 PlayerShip::ComputeTransform() const
{
    return GetModelOffset() * m_Orientation.toAffine() * translation(m_Position);
}

void PlayerShip::SetPose(const float3& position, float yaw, float pitch, float roll)
{
    // Asteroids.exe: 0x140046450
    m_Position = position;
    m_Orientation = rotationQuat(float3(pitch, yaw, roll));
    UpdateBasis();
    if (m_CargoShip)
        m_CargoShip->SetTransform(ComputeTransform());
}

void PlayerShip::SetSpeed(float speed)
{
    // Asteroids.exe: 0x140046550
    m_Speed = speed;
    m_Deceleration = 0.f;
}

void PlayerShip::Stop()
{
    // Asteroids.exe: 0x140046560
    m_Deceleration = -m_Speed / m_TimeToStop;
    m_Thrust = 0.f;
}

float3 PlayerShip::Move(float /*time*/, float dt, bool freezePosition)
{
    // Asteroids.exe: 0x1400465F0
    const float oldSpeed = m_Speed;
    const float damping = std::max(fabsf(oldSpeed) * 0.0003f, 1.f);    // slower turning at high speed
    m_Speed = oldSpeed + dt * m_ThrottleInput * m_AccelerationScale;

    const float3 deltaEuler(
        m_RotationSensitivity * m_PitchInput * dt / damping,
        m_RotationSensitivity * m_YawInput * dt / damping,
        m_RollSensitivity * m_RollInput * dt);
    const quat q = m_Orientation * rotationQuat(deltaEuler);
    m_Orientation = q / length(q);

    m_PitchInput = 0.f;
    m_YawInput = 0.f;
    m_RollInput = 0.f;
    m_Thrust = std::min(std::max(-1.f, m_ThrottleInput), 1.f);

    if (m_Deceleration != 0.f)
    {
        const float step = m_Deceleration * dt;
        if (fabsf(step) <= fabsf(m_Speed))
            m_Speed += step;
        else
        {
            m_Speed = 0.f;
            m_Deceleration = 0.f;
        }
    }

    float3 position = m_Position;
    if (!freezePosition)
        position += (dt * m_Speed) * m_Forward;     // forward vector of the previous UpdateBasis
    return position;
}

void PlayerShip::Animate(float time, float dt)
{
    // Asteroids.exe: 0x140046590
    const float3 position = Move(time, dt, false);
    Update(time, dt, position, position);
}

void PlayerShip::Update(float time, float dt, const float3& desiredPosition, const float3& actualPosition)
{
    // Asteroids.exe: 0x140046820
    UpdateBasis();

    float3 p = actualPosition;
    const float3& mn = m_PlayVolumeMin;
    const float3& mx = m_PlayVolumeMax;
    if (mn.x <= mx.x && mn.y <= mx.y && mn.z <= mx.z)
    {
        // Y is clamped, X and Z wrap around (torus).
        const float y = std::min(std::max(mn.y, p.y), mx.y);
        m_BoundaryCorrection = float3(0.f);
        if (p.x < mn.x)
            m_BoundaryCorrection.x = mx.x - mn.x;
        else if (p.x > mx.x)
            m_BoundaryCorrection.x = -(mx.x - mn.x);
        if (p.z < mn.z)
            m_BoundaryCorrection.z = mx.z - mn.z;
        else if (p.z > mx.z)
            m_BoundaryCorrection.z = -(mx.z - mn.z);
        p = float3(p.x + m_BoundaryCorrection.x, y + m_BoundaryCorrection.y, p.z + m_BoundaryCorrection.z);
    }

    // Lose speed when the collision response deflected the move.
    if (length(desiredPosition - m_Position) > 0.f)
    {
        const float3 moved = actualPosition - m_Position;
        const float3 velocity = m_Speed * m_Forward;
        const float c = dot(normalize(moved), normalize(velocity));
        m_Speed *= 1.f - std::clamp(1.f - c, 0.f, 1.f) * 0.4f;
    }

    const float t = std::max(fabsf(m_Thrust), 0.f);

    // Glow intensities. Move() already cleared the yaw/pitch inputs, so the front glows stay dark and
    // the rear glows follow |thrust| (2018 behaviour; the formulas are kept).
    if (m_CargoShip)
    {
        m_CargoShip->SetGlowIntensity(0, m_YawInput > 0.f ? m_YawInput : 0.f);
        m_CargoShip->SetGlowIntensity(1, m_YawInput > 0.f ? m_YawInput : 0.f);
        m_CargoShip->SetGlowIntensity(2, std::max(-m_YawInput, 0.f));
        m_CargoShip->SetGlowIntensity(3, std::max(-m_YawInput, 0.f));
        const float top = std::min(1.2f, m_PitchInput + 1.f) * t;
        const float bottom = std::min(1.2f, 1.f - m_PitchInput) * t;
        m_CargoShip->SetGlowIntensity(4, top);
        m_CargoShip->SetGlowIntensity(5, bottom);
        m_CargoShip->SetGlowIntensity(6, top);
        m_CargoShip->SetGlowIntensity(7, bottom);
    }

    if (m_JetSound)
    {
        if (t == 0.f)
            m_JetSound->Stop();
        else
        {
            m_JetSound->Play();
            m_JetSound->SetFrequencyRatio(t * 1.1f);
        }
    }

    // Nav-light blink pattern: two flashes per 1.3 s cycle (stern) and per 1.1 s cycle (top).
    const int a = int(time * 19.f / 1.3f) % 19;
    const int b = int(time * 19.f / 1.1f) % 19;
    const float blinkA = (a == 0 || a == 3) ? 1.f : 0.f;
    const float blinkB = (b == 10 || b == 15) ? 1.f : 0.f;

    m_Position = p;
    if (m_CargoShip)
    {
        m_CargoShip->SetNavLightState(float3(blinkA, blinkB, 0.f));
        m_CargoShip->SetTransform(ComputeTransform());
        m_CargoShip->Update(time, dt);
    }
}
