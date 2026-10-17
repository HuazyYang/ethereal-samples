#pragma once

#include <waveworks/GFSDK_WaveWorks_FloatTypes.h>
#include <GLFW/glfw3.h>

#include <array>

#include <map>

// FPS-style camera
class FPSCamera 
{
public:
	void SetProjection(float fovy, float aspect, float znear, float zfar);
	void LookAt(gfsdk_float3 posCamera, gfsdk_float3 posTarget, gfsdk_float3 up = { 0.f, 1.f, 0.f });
	void SetSpeeds(float movementSpeed, float rotationSpeed) { moveSpeed = movementSpeed; rotateSpeed = rotationSpeed; }

    void KeyboardUpdate(int key, int scancode, int action, int mods);
    void MousePosUpdate(double xpos, double ypos);
    void MouseButtonUpdate(int button, int action, int mods);
    void Animate(float deltaT);

	gfsdk_float3 GetCameraPosition() const { return cameraPos; }
	gfsdk_float4x4 GetProjection()  const { return matProjection; }
	gfsdk_float4x4 GetWorldToView() const { return matWorldToView; }
	gfsdk_float4x4 GetWorldToClip() const { return matWorldToClip; }

protected:
	void updateWorldToView();
	void updateWorldToClip();

	gfsdk_float4x4 matWorldToView;    // transform from world space to screen UV [0, 1]
	gfsdk_float4x4 matProjection;     // projection matrix (view space to clip space)
	gfsdk_float4x4 matWorldToClip;    // transform from world space to clip space [-1, 1] (WorldToView * Projection)

    float moveSpeed = 1.f;      // movement speed in units/second
    float rotateSpeed = .005f;  // mouse sensitivity in radians/pixel

	gfsdk_float2 mousePos;
	gfsdk_float2 mousePosPrev;

	gfsdk_float3 cameraPos;   // in worldspace
	gfsdk_float3 cameraDir;   // normalized
	gfsdk_float3 cameraUp;    // normalized
	gfsdk_float3 cameraRight; // normalized

    typedef enum
    {
        MoveUp,
        MoveDown,
        MoveLeft,
        MoveRight,
        MoveForward,
        MoveBackward,

        YawRight,
        YawLeft,
        PitchUp,
        PitchDown,
        RollLeft,
        RollRight,

        SpeedUp,
        SlowDown,

        KeyboardControlCount,
    } KeyboardControls;

    typedef enum
    {
        Left,
        Middle,
        Right,

        MouseButtonCount,
        MouseButtonFirst = Left,
    } MouseButtons;

    const std::map<int, int> keyboardMap = {
        { GLFW_KEY_E, KeyboardControls::MoveUp },
        { GLFW_KEY_Q, KeyboardControls::MoveDown },
        { GLFW_KEY_A, KeyboardControls::MoveLeft },
        { GLFW_KEY_D, KeyboardControls::MoveRight },
        { GLFW_KEY_W, KeyboardControls::MoveForward },
        { GLFW_KEY_S, KeyboardControls::MoveBackward },
        { GLFW_KEY_LEFT, KeyboardControls::YawLeft },
        { GLFW_KEY_RIGHT, KeyboardControls::YawRight },
        { GLFW_KEY_UP, KeyboardControls::PitchUp },
        { GLFW_KEY_DOWN, KeyboardControls::PitchDown },
        { GLFW_KEY_Z, KeyboardControls::RollLeft },
        { GLFW_KEY_C, KeyboardControls::RollRight },
        { GLFW_KEY_LEFT_SHIFT, KeyboardControls::SpeedUp },
        { GLFW_KEY_RIGHT_SHIFT, KeyboardControls::SpeedUp },
        { GLFW_KEY_LEFT_CONTROL, KeyboardControls::SlowDown },
        { GLFW_KEY_RIGHT_CONTROL, KeyboardControls::SlowDown },
    };

    const std::map<int, int> mouseButtonMap = {
        { GLFW_MOUSE_BUTTON_LEFT, MouseButtons::Left },
        { GLFW_MOUSE_BUTTON_MIDDLE, MouseButtons::Middle },
        { GLFW_MOUSE_BUTTON_RIGHT, MouseButtons::Right },
    };

    std::array<bool, KeyboardControls::KeyboardControlCount> keyboardState = { false };
    std::array<bool, MouseButtons::MouseButtonCount> mouseButtonState = { false };

private:
	gfsdk_float4x4 perspective(const float fovy, const float aspect, const float zNear, const float zFar);
	gfsdk_float4x4 lookAtLH(const gfsdk_float3& eyePos, const gfsdk_float3& targetPoint, const gfsdk_float3& eyeUp);
	gfsdk_float4x4 rotate(const float rotAngle, const gfsdk_float3& rotAxis);
};
