#pragma once

// Asteroids.exe: ThirdPersonCamera (vtable 0x14025B650, ctor 0x140062170, Animate 0x1400624B0)
//
// The chase camera that follows the player ship. It keeps a short history of the ship transform and aims
// at the ship from a point that lags 0.7 s behind it, so that turns and accelerations are visible.
// Created by FeatureDemo::SceneLoaded as ThirdPersonCamera(ship, 50, 0, 0.4).
//
// deviation: class_map.md maps this class to donut's ThirdPersonCamera ("adapt"). The 2018 behaviour
// (ship-relative orbit, lagged chase point, gamepad impulses) has little in common with donut's orbit
// camera, so it is reconstructed on top of donut::app::BaseCamera instead.

#include <donut/app/Camera.h>

#include <list>

class PlayerShip;

class ThirdPersonCamera : public donut::app::BaseCamera
{
public:
    ThirdPersonCamera(PlayerShip* target, float distance, float yaw, float pitch);

    // Slot 0 of the 2018 vtable is a no-op: the camera ignores the keyboard.
    void KeyboardUpdate(int key, int scancode, int action, int mods) override { }
    void MousePosUpdate(double xpos, double ypos) override;                    // 0x140063200
    void MouseButtonUpdate(int button, int action, int mods) override;         // 0x140063110
    void MouseScrollUpdate(double xoffset, double yoffset) override;           // 0x140063230
    void JoystickButtonUpdate(int button, bool pressed) override;              // 0x140062E60
    void JoystickUpdate(int axis, float value) override;                       // 0x140062E30
    void Animate(float deltaT) override;                                       // 0x1400624B0

    void ResetRotation();                                                      // 0x1400632E0
    void SetDistance(float distance) { m_Distance = distance; }                // 0x140063350
    void SetRotation(float yaw, float pitch);                                  // 0x140063360
    void ClearHistory() { m_History.clear(); }                                 // 0x140063270

    PlayerShip* GetTarget() const { return m_Target; }
    float GetDistance() const { return m_Distance; }

private:
    struct HistoryEntry
    {
        float age = 0.f;
        dm::affine3 transform = dm::affine3::identity();
    };

    // 0x140062EB0: ages the history, drops entries older than the lag (keeping one for interpolation),
    // and appends the current ship transform.
    void UpdateHistory(float deltaT, const dm::float3& frameOffset);

    static constexpr float c_MinDistance = 27.93f;
    static constexpr float c_MaxDistance = 300.f;
    static constexpr float c_MaxPitch = 1.3345f;
    static constexpr float c_MouseRotateSpeed = 0.005f;
    static constexpr float c_MaxLagOffset = 60.f;

    std::list<HistoryEntry> m_History;              // +160
    dm::float2 m_MousePos = 0.f;                    // +176
    dm::float2 m_MousePosPrev = 0.f;                // +184
    dm::float2 m_LeftDragStart = 0.f;               // +192
    dm::float2 m_RightDragStart = 0.f;              // +200
    PlayerShip* m_Target = nullptr;                 // +208
    dm::quat m_Rotation = dm::quat::identity();     // +216, from (pitch, yaw, 0)
    float m_Distance = 50.f;                        // +232
    float m_LagTime = 0.7f;                         // +236
    float m_Yaw = 0.f;                              // +240
    float m_Pitch = 0.f;                            // +244
    float m_DefaultYaw = 0.f;                       // +248
    float m_DefaultPitch = 0.f;                     // +252
    dm::float3 m_Up = dm::float3(0.f, 1.f, 0.f);    // +256, smoothed towards the ship's up vector
    float m_JoystickYaw = 0.f;                      // +268, axis 2 (per-frame impulse)
    float m_JoystickPitch = 0.f;                    // +272, axis 3
    float m_JoystickDistance = 0.f;                 // +276, buttons A/B
    // +280: keyboard map (same table as the FPS camera) - unused because KeyboardUpdate is a no-op.
    // +296: mouse button map {left -> 0, middle -> 1, right -> 2}.
    bool m_MouseButtons[3] = {};                    // +326..+328
};
