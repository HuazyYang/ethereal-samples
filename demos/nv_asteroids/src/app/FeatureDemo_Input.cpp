// Asteroids.exe: FeatureDemo per-frame update and input handling.
//   Animate 0x14002A4B0, KeyboardUpdate 0x14002EC20, MousePosUpdate 0x140031900, MouseScrollUpdate 0x140031960,
//   MouseButtonUpdate 0x1400318E0, JoystickButtonUpdate 0x14002EB90, JoystickAxisUpdate 0x14002EB00,
//   joystick axis handler 0x1400380D0, joystick button handler 0x1400381C0.

#include "app/FeatureDemo.h"
#include "app/AppGlobals.h"
#include "app/ThirdPersonCamera.h"
#include "app/UIData.h"

#include "audio/SoundEngine.h"
#include "scene/PlayerShip.h"
#include "scene/SpaceScene.h"

#include "passes/ToneMappingPass2018.h"

#include <donut/core/log.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

using namespace donut::math;

namespace
{
    // Fixed simulation step while a replay is recorded or played back (0x140259448).
    constexpr float c_ReplayTimeStep = 1.f / 60.f;

    // Radius of the sphere swept against the asteroids for the player ship (0x1402594C4).
    constexpr float c_ShipCollisionRadius = 31.f;

    int64_t NowNanoseconds()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    float TriggerToUnit(float value)
    {
        // Triggers report [-1, 1]; the demo maps them to [0, 1].
        return std::min(std::max(0.f, value * 0.5f + 0.5f), 1.f);
    }
}

// Busy-waits (with Sleep(0)) until 1/60 s has passed since the previous limited frame.
// The timestamps were function-local statics in Animate (xmmword_1402DF228).
void FeatureDemo::LimitFrameRate()
{
    static int64_t s_PreviousFrame = 0;

    if (demo::g_SkipNextFrameLimit)
    {
        demo::g_SkipNextFrameLimit = false;
    }
    else
    {
        int64_t now = NowNanoseconds();
        while (double(int32_t(now) - int32_t(s_PreviousFrame)) * 1e-9 < double(c_ReplayTimeStep))
        {
            now = NowNanoseconds();
            Sleep(0);
        }
    }

    s_PreviousFrame = NowNanoseconds();
}

void FeatureDemo::Animate(float elapsedTimeSeconds)
{
    float elapsedTime = elapsedTimeSeconds;

    // Benchmark statistics are collected while the benchmark replay plays back. When it stops, the
    // result popup is shown and benchmark mode ends.
    bool benchmark = demo::g_Options.benchmark;
    if (benchmark)
    {
        if (m_Replay.IsPlaying())
        {
            ++demo::g_BenchmarkFrameCount;
            demo::g_BenchmarkTotalTime += double(elapsedTimeSeconds);
        }
        else if (demo::g_BenchmarkFrameCount > 0)
        {
            m_UI->showBenchmarkResult = true;
            benchmark = false;
            // deviation (developer aid): the binary only showed the result in the popup; also log it.
            const double averageFrameTime = demo::g_BenchmarkTotalTime / double(demo::g_BenchmarkFrameCount);
            donut::log::info("Benchmark result: %d frames, average frame time = %.2f ms, average FPS = %.1f",
                demo::g_BenchmarkFrameCount, averageFrameTime * 1000.0, 1.0 / averageFrameTime);
            // The counters of the F-key overlay in the last replayed frame (to compare the workload with the original).
            donut::log::info("Benchmark workload: total asteroids %llu, drawn triangles %llu, max LOD triangles %llu",
                (unsigned long long)m_UI->asteroidsDrawn, (unsigned long long)m_UI->drawnTriangles,
                (unsigned long long)m_UI->maxLodTriangles);
            demo::g_Options.benchmark = false;
        }
    }

    // Replays run on a fixed 60 Hz step; outside of benchmarks the frame rate is limited to match.
    if (m_Replay.IsPlaying() || m_Replay.IsRecording())
    {
        if (!benchmark)
            LimitFrameRate();
        elapsedTime = c_ReplayTimeStep;
    }

    float frameStep = 0.f;
    if (!m_UI->paused)
    {
        frameStep = elapsedTime;
        m_CurrentTime += elapsedTime;
    }
    m_ElapsedTime = frameStep;

    if (m_Scene)
    {
        PlayerShip* ship = m_Scene->GetPlayerShip();

        ship->SetThrottle(float(m_KeyThrottle) + m_JoystickThrottle);
        ship->SetYawInput(float(m_KeyYaw) * 0.75f + m_JoystickYaw);
        ship->SetPitchInput(float(m_KeyPitch) + m_JoystickPitch);
        ship->SetRollInput((float(m_KeyRoll) + m_JoystickRoll) - float(m_KeyYaw));

        m_JoystickThrottle = 0.f;
        m_JoystickYaw = 0.f;
        m_JoystickPitch = 0.f;
        m_JoystickRoll = 0.f;

        // Note: the ship always advances by the unpaused step; only the demo clock stops when paused.
        if (!m_UI->shipCollisions)
        {
            ship->Animate(m_CurrentTime, elapsedTime);
            m_CollisionIntensity = 0.f;
        }
        else
        {
            const float3 currentPosition = ship->GetPosition();
            const float3 targetPosition = ship->Move(m_CurrentTime, elapsedTime, m_UI->freezeShipPosition);
            const float3 resolvedPosition = m_Scene->ResolveCollision(currentPosition, targetPosition,
                c_ShipCollisionRadius);
            ship->Update(m_CurrentTime, elapsedTime, targetPosition, resolvedPosition);

            const float3 correction = targetPosition - resolvedPosition;
            float penetration = length(correction);
            if (penetration > 0.f)
                m_CollisionNormal = correction / penetration;

            if (penetration > 0.1f)
                m_Scene->GetPlayerShip()->PlayShieldSound();

            // The shield flash decays with a 0.1 s time constant.
            const float decayed = expf(elapsedTime / -0.1f) * m_CollisionIntensity;
            if (penetration <= decayed)
                penetration = decayed;
            m_CollisionIntensity = penetration;
            if (penetration < 0.0001f)
                m_CollisionIntensity = 0.f;
        }
    }

    m_FirstPersonCamera.SetMoveSpeed(powf(2.f, m_UI->cameraSpeedExponent));
    m_ActiveCamera->Animate(elapsedTime);

    if (m_ToneMappingPass)
        m_ToneMappingPass->AdvanceFrame(elapsedTime);

    if (m_ActiveSequence)
    {
        Animation::Sequence& sequence = *m_ActiveSequence;

        // unresolved: like the binary, the current time is only reset by the first update after a
        // restart if the previous run has not finished; a second "Play Opening Sequence" right after a
        // finished one therefore ends immediately.
        if (sequence.IsFinished())
            sequence.SetStartTime(m_CurrentTime);
        else
            sequence.SetCurrentTime(m_CurrentTime - sequence.GetStartTime());

        ApplyAnimation(sequence, std::nullopt, false);

        if (sequence.IsFinished())
        {
            m_ActiveSequence = nullptr;

            if (m_Replay.GetMode() != InputReplay::Mode::Playback)
                return;

            // The opening sequence is over: start the replay from a deterministic state.
            m_CurrentTime = 0.f;
            demo::g_ReplayFrameIndex = 0;
            demo::g_RandomEngine.seed(std::mt19937::default_seed);
            m_Replay.SetStartFrame(0);
            m_UI->enableVsync = !demo::g_Options.benchmark;
        }
    }

    // Feed the recorded gamepad input through the same handlers as live input.
    if (m_Replay.IsPlaying())
    {
        const InputReplay::Frame frame = m_Replay.GetFrame(demo::g_ReplayFrameIndex);

        for (uint32_t button = 0; button < InputReplay::c_NumButtons; button++)
            HandleJoystickButton(int(button), ((frame.buttons >> button) & 1) != 0);

        for (uint32_t axis = 0; axis < InputReplay::c_NumAxes; axis++)
            HandleJoystickAxis(int(axis), frame.axes[axis]);
    }
}

bool FeatureDemo::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    if (!IsSceneLoaded())
        return false;

    bool handled = true;

    // Ship controls in the third-person camera mode. Keys hold -1/0/+1 while pressed.
    if (action <= GLFW_PRESS && m_UI->cameraMode == 1)
    {
        const int down = (action == GLFW_PRESS) ? 1 : 0;
        switch (key)
        {
        case GLFW_KEY_A:
        case GLFW_KEY_LEFT:
            m_KeyYaw = down;
            return true;
        case GLFW_KEY_D:
        case GLFW_KEY_RIGHT:
            m_KeyYaw = -down;
            return true;
        case GLFW_KEY_E:
            m_KeyRoll = down;
            return true;
        case GLFW_KEY_Q:
            m_KeyRoll = -down;
            return true;
        case GLFW_KEY_S:
            m_KeyThrottle = -down;
            return true;
        case GLFW_KEY_W:
            m_KeyThrottle = down;
            return true;
        case GLFW_KEY_DOWN:
            m_KeyPitch = -down;
            return true;
        case GLFW_KEY_UP:
            m_KeyPitch = down;
            return true;
        default:
            break;
        }
    }

    if (action == GLFW_PRESS)
    {
        if (key == GLFW_KEY_GRAVE_ACCENT)
        {
            m_UI->showGui = !m_UI->showGui;
        }
        else if (key == GLFW_KEY_PAUSE)
        {
            m_UI->paused = !m_UI->paused;
        }
        else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)
        {
            const int digit = key - GLFW_KEY_0;

            if (mods & GLFW_MOD_CONTROL)
            {
                SaveCameraPreset(digit);
            }
            else if ((mods & GLFW_MOD_ALT) && size_t(digit) < m_CameraPresets.size())
            {
                m_UI->cameraMode = 0;
                ApplyCameraPreset(m_CameraPresets[digit]);
            }

            if (mods == 0 && digit <= 6)
                m_UI->staticLodIndex = digit;
        }
        else
        {
            switch (key)
            {
            case GLFW_KEY_SPACE:
                if (m_Scene)
                    m_Scene->GetPlayerShip()->Stop();
                break;

            case GLFW_KEY_T:
                if (m_UI->cameraMode == 1)
                {
                    // Switch to the free camera at the current chase camera position.
                    m_UI->cameraMode = 0;
                    if (m_ShipCamera)
                    {
                        const float3 position = m_ShipCamera->GetPosition();
                        m_FirstPersonCamera.LookAt(position, position + m_ShipCamera->GetDir(), float3(0.f, 1.f, 0.f));
                    }
                }
                else if (m_UI->cameraMode == 0)
                {
                    m_UI->cameraMode = 1;
                }
                break;

            case GLFW_KEY_O:
                // Reset to the end of the opening sequence and stop the music.
                ResetOpeningSequence();
                if (audio::Sound* music = m_Scene ? m_Scene->GetAmbienceMusic() : nullptr)
                {
                    music->Stop();
                    music->Rewind();
                }
                break;

            case GLFW_KEY_P:
                PlayOpeningSequence();
                if (audio::Sound* music = m_Scene ? m_Scene->GetAmbienceMusic() : nullptr)
                    music->Play();
                break;

            case GLFW_KEY_F:
                m_UI->showCounters = !m_UI->showCounters;
                break;

            case GLFW_KEY_K:
                m_UI->wireframe = !m_UI->wireframe;
                break;

            case GLFW_KEY_L:
                m_UI->dynamicLod = !m_UI->dynamicLod;
                break;

            case GLFW_KEY_V:
                m_UI->enableVsync = !m_UI->enableVsync;
                break;

            case GLFW_KEY_ESCAPE:
                m_UI->showConfirmExit = !m_UI->showConfirmExit;
                break;

            default:
                handled = false;
                break;
            }
        }
    }

    m_ActiveCamera->KeyboardUpdate(key, scancode, action, mods);
    return handled;
}

bool FeatureDemo::MousePosUpdate(double xpos, double ypos)
{
    m_ActiveCamera->MousePosUpdate(xpos, ypos);
    m_MousePosition = int2(int(xpos), int(ypos));
    return true;
}

bool FeatureDemo::MouseScrollUpdate(double xoffset, double yoffset)
{
    m_ActiveCamera->MouseScrollUpdate(xoffset, yoffset);
    return true;
}

bool FeatureDemo::MouseButtonUpdate(int button, int action, int mods)
{
    m_ActiveCamera->MouseButtonUpdate(button, action, mods);
    return true;
}

bool FeatureDemo::JoystickButtonUpdate(int button, bool pressed)
{
    // Live gamepad input is ignored while a replay plays back, and recorded while one records.
    if (m_Replay.IsPlaying())
        return true;

    if (m_Replay.IsRecording() && button < 16)
    {
        InputReplay::Frame& frame = m_Replay.GetFrame(demo::g_ReplayFrameIndex);
        if (pressed)
            frame.buttons = uint16_t(frame.buttons | (1u << (button & 15)));
        else
            frame.buttons = uint16_t(frame.buttons & ~(1u << (button & 15)));
    }

    return HandleJoystickButton(button, pressed);
}

bool FeatureDemo::JoystickAxisUpdate(int axis, float value)
{
    if (m_Replay.IsPlaying())
        return true;

    if (m_Replay.IsRecording() && axis < int(InputReplay::c_NumAxes))
        m_Replay.GetFrame(demo::g_ReplayFrameIndex).axes[axis] = value;

    return HandleJoystickAxis(axis, value);
}

bool FeatureDemo::HandleJoystickAxis(int axis, float value)
{
    if (!m_Scene)
        return false;

    switch (axis)
    {
    case 0: // left stick X: roll and yaw
        m_JoystickRoll += value;
        m_JoystickYaw -= value * 0.75f;
        return true;
    case 1: // left stick Y: pitch
        m_JoystickPitch += value;
        return true;
    case 4: // left trigger: decelerate
        m_JoystickThrottle -= TriggerToUnit(value);
        return true;
    case 5: // right trigger: accelerate
        m_JoystickThrottle += TriggerToUnit(value);
        return true;
    default: // right stick: camera
        if (!m_ShipCamera)
            return false;
        m_ShipCamera->JoystickUpdate(axis, value);
        return true;
    }
}

bool FeatureDemo::HandleJoystickButton(int button, bool pressed)
{
    if (!m_Scene || button >= int(InputReplay::c_NumButtons))
        return false;

    const bool justPressed = pressed && !m_JoystickButtons[button];
    m_JoystickButtons[button] = pressed;

    switch (button)
    {
    case 2: // X: stop the ship
        if (justPressed)
            m_Scene->GetPlayerShip()->Stop();
        return true;
    case 4: // LB: roll
        if (pressed)
            m_JoystickRoll -= 1.f;
        return true;
    case 5: // RB: roll
        if (pressed)
            m_JoystickRoll += 1.f;
        return true;
    case 9: // reset the camera rotation
        if (justPressed && m_ShipCamera)
            m_ShipCamera->ResetRotation();
        return true;
    case 10: // D-pad up: wireframe
        if (justPressed)
            m_UI->wireframe = !m_UI->wireframe;
        return true;
    case 11: // D-pad right: temporal AA (toggles between None and TemporalAA)
        if (justPressed)
            m_UI->aaMode = (m_UI->aaMode == AntiAliasingMode::None) ? AntiAliasingMode::TemporalAA : AntiAliasingMode::None;
        return true;
    case 12: // D-pad down: visualize LODs
        if (justPressed)
            m_UI->visualizeLods = !m_UI->visualizeLods;
        return true;
    case 13: // D-pad left: text
        if (justPressed)
            m_UI->showCounters = !m_UI->showCounters;
        return true;
    default: // A/B and the rest go to the chase camera
        if (!m_ShipCamera)
            return false;
        m_ShipCamera->JoystickButtonUpdate(button, pressed);
        return true;
    }
}

void FeatureDemo::ApplyCameraPreset(const CameraPreset& preset)
{
    m_FirstPersonCamera.LookAt(preset.position, preset.lookAt, preset.up);
}
