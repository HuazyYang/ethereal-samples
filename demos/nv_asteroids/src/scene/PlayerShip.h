#pragma once

// Flight model of the player's ship: throttle/pitch/yaw/roll inputs, orientation quaternion,
// speed with "time to stop" braking, play-volume wrapping, engine glow / jet sound / nav-light
// driving of the CargoShip, and the shield impact sound.
//
// Asteroids.exe: PlayerShip (0xA8 bytes, owned at SpaceScene+256; ctor 0x140045A40, controls
// 0x140045CA0, sounds 0x140045EF0, Move 0x1400465F0, Update 0x140046820). No RTTI exists for it.

#include <donut/core/math/math.h>
#include <nvrhi/core/autoptr.h>

#include <cfloat>
#include <filesystem>
#include <memory>

namespace Json { class Value; }
namespace donut::vfs { class IFileSystem; }
namespace audio { class Sound; }

class CargoShip;

class PlayerShip
{
public:
    explicit PlayerShip(CargoShip* cargoShip);
    ~PlayerShip();

    // fscene "controls": top_speed_forward, top_speed_reverse, acceleration_scale, rotation_sensitivity,
    // roll_sensitivity, time_to_stop, play_volume_min, play_volume_max.
    void LoadControls(const Json::Value& controls);
    // fscene "player_ship"[0]: jetSound (looping), jetSoundVolume, shieldSound (one-shot), shieldSoundVolume.
    void LoadSounds(const Json::Value& playerShipArray, nvrhi::AutoPtr<donut::vfs::IFileSystem> fs,
        const std::filesystem::path& mediaPath);

    // 0x140046450. Euler angles are in radians (the fscene "yaw"/"pitch" values are passed unconverted).
    void SetPose(const donut::math::float3& position, float yaw, float pitch, float roll);
    void SetSpeed(float speed);     // "PlayerShipSpeed" animation track
    void Stop();                    // Space / gamepad X: decelerate to 0 within time_to_stop

    void SetThrottle(float value) { m_ThrottleInput = value; }
    void SetPitchInput(float value) { m_PitchInput = value; }
    void SetYawInput(float value) { m_YawInput = value; }
    void SetRollInput(float value) { m_RollInput = value; }

    // Integrates speed and orientation and returns the desired new position (not applied yet).
    // Clears the rotation inputs (not the throttle).
    donut::math::float3 Move(float time, float dt, bool freezePosition);
    // Applies the collision-resolved position: play-volume wrap/clamp, speed loss on deflection,
    // glow intensities, jet sound, nav-light blinking and the CargoShip transform/update.
    void Update(float time, float dt, const donut::math::float3& desiredPosition, const donut::math::float3& actualPosition);
    // Move + Update without collision.
    void Animate(float time, float dt);

    // Restarts the shield sound unless it is still playing (FeatureDemo calls it on impacts > 0.1).
    void PlayShieldSound();

    CargoShip* GetCargoShip() const { return m_CargoShip; }
    const donut::math::float3& GetPosition() const { return m_Position; }
    const donut::math::quat& GetOrientation() const { return m_Orientation; }
    const donut::math::float3& GetForward() const { return m_Forward; }
    const donut::math::float3& GetUp() const { return m_Up; }
    float GetSpeed() const { return m_Speed; }
    float GetThrust() const { return m_Thrust; }
    const donut::math::float3& GetBoundaryCorrection() const { return m_BoundaryCorrection; }
    const donut::math::float3& GetPlayVolumeMin() const { return m_PlayVolumeMin; }
    const donut::math::float3& GetPlayVolumeMax() const { return m_PlayVolumeMax; }

    // Model offset applied before the ship orientation (global xmmword_1402DF278: translation (0, 5, 0)).
    static donut::math::affine3 GetModelOffset();

private:
    void UpdateBasis();
    donut::math::affine3 ComputeTransform() const;

    CargoShip* m_CargoShip = nullptr;
    donut::math::float3 m_Forward = donut::math::float3(1.f, 0.f, 0.f);
    donut::math::float3 m_Up = donut::math::float3(0.f, 1.f, 0.f);
    float m_Speed = 0.f;
    float m_Deceleration = 0.f;
    float m_ThrottleInput = 0.f;
    float m_PitchInput = 0.f;
    float m_YawInput = 0.f;
    float m_RollInput = 0.f;
    donut::math::quat m_Orientation = donut::math::quat::identity();
    donut::math::float3 m_Position = 0.f;
    donut::math::float3 m_PlayVolumeMin = donut::math::float3(FLT_MAX);     // empty volume = no wrapping
    donut::math::float3 m_PlayVolumeMax = donut::math::float3(-FLT_MAX);
    donut::math::float3 m_BoundaryCorrection = 0.f;
    float m_Thrust = 0.f;
    // The two top speeds are parsed but never used by the 2018 flight model (speed is unbounded).
    float m_TopSpeedForward = 1000.f;
    float m_TopSpeedReverse = 200.f;
    float m_AccelerationScale = 200.f;
    float m_RotationSensitivity = 1.f;
    float m_RollSensitivity = 1.f;
    float m_TimeToStop = 0.5f;
    std::unique_ptr<audio::Sound> m_JetSound;
    std::unique_ptr<audio::Sound> m_ShieldSound;
};
