#include "Camera.h"

void FPSCamera::SetProjection(float fovy, float aspect, float znear, float zfar)
{
    matProjection = perspective(fovy, aspect, znear, zfar);
    updateWorldToClip();
}

void FPSCamera::updateWorldToClip()
{
    matWorldToClip = matProjection * matWorldToView;
}

void FPSCamera::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    if (keyboardMap.find(key) == keyboardMap.end())
    {
        return;
    }

    auto cameraKey = keyboardMap.at(key);
    if (action == GLFW_PRESS || action == GLFW_REPEAT)
    {
        keyboardState[cameraKey] = true;
    } else {
        keyboardState[cameraKey] = false;
    }
}

void FPSCamera::MousePosUpdate(double xpos, double ypos)
{
    mousePos = { float(xpos), float(ypos) };
}

void FPSCamera::MouseButtonUpdate(int button, int action, int mods)
{
    if (mouseButtonMap.find(button) == mouseButtonMap.end())
    {
        return;
    }

    auto cameraButton = mouseButtonMap.at(button);
    if (action == GLFW_PRESS)
    {
        mouseButtonState[cameraButton] = true;
    } else {
        mouseButtonState[cameraButton] = false;
    }
}

void FPSCamera::updateWorldToView()
{
    matWorldToView = lookAtLH(cameraPos, cameraPos + cameraDir, cameraUp);
    updateWorldToClip();
}

void FPSCamera::LookAt(gfsdk_float3 cameraPos, gfsdk_float3 cameraTarget, gfsdk_float3 cameraUp)
{
    this->cameraPos = cameraPos;
    this->cameraDir = normalize(cameraTarget - cameraPos);
    this->cameraUp = normalize(cameraUp);
    this->cameraRight = normalize(cross(this->cameraUp, this->cameraDir));

    updateWorldToView();
}

void FPSCamera::Animate(float deltaT)
{
    // track mouse delta
    gfsdk_float2 mouseMove = mousePos - mousePosPrev;
    mousePosPrev = mousePos;

    bool cameraDirty = false;
	float yawRotation = 0;
	float pitchRotation = 0;
	float rollRotation = 0;

    // handle mouse rotation first
    // this will affect the movement vectors in the world matrix, which we use below
    if (mouseButtonState[MouseButtons::Left] && (mouseMove.x || mouseMove.y))
    {
		yawRotation = -rotateSpeed * mouseMove.x;
		pitchRotation = -rotateSpeed * mouseMove.y;
        cameraDirty = true;
    }

    // handle keyboard roll next
    if (keyboardState[KeyboardControls::RollLeft] ||
        keyboardState[KeyboardControls::RollRight])
    {
		rollRotation = - (float)keyboardState[KeyboardControls::RollLeft] * rotateSpeed * 2.0f +
                       (float)keyboardState[KeyboardControls::RollRight] * rotateSpeed * 2.0f;
        cameraDirty = true;
    }

    // handle translation
    float moveStep = deltaT * moveSpeed;
	gfsdk_float3 cameraMoveVec = { 0.f, 0.f, 0.f };

    if (keyboardState[KeyboardControls::SpeedUp])
        moveStep *= 10.f;

    if (keyboardState[KeyboardControls::SlowDown])
        moveStep *= .1f;

    if (keyboardState[KeyboardControls::MoveForward])
    {
        cameraDirty = true;
        cameraMoveVec += moveStep * cameraDir;
    }

    if (keyboardState[KeyboardControls::MoveBackward])
    {
        cameraDirty = true;
        cameraMoveVec += -moveStep * cameraDir;
    }

    if (keyboardState[KeyboardControls::MoveLeft])
    {
        cameraDirty = true;
        cameraMoveVec += -moveStep * cameraRight;
    }

    if (keyboardState[KeyboardControls::MoveRight])
    {
        cameraDirty = true;
        cameraMoveVec += moveStep * cameraRight;
    }

    if (keyboardState[KeyboardControls::MoveUp])
    {
        cameraDirty = true;
        cameraMoveVec += moveStep * cameraUp;
    }

    if (keyboardState[KeyboardControls::MoveDown])
    {
        cameraDirty = true;
        cameraMoveVec += -moveStep * cameraUp;
    }

    if (cameraDirty)
    {
        cameraPos += cameraMoveVec;
		gfsdk_float4x4 rotationMatrix = rotate(yawRotation, gfsdk_make_float3(0, 1.0f, 0)) * rotate(pitchRotation, cameraRight) * rotate(rollRotation, cameraDir);
        cameraDir = gfsdk_make_float3(normalize(rotationMatrix * gfsdk_make_float4(cameraDir, 0.f)));
        cameraUp = gfsdk_make_float3(normalize(rotationMatrix * gfsdk_make_float4(cameraUp, 0.f)));
        cameraRight = normalize(cross(cameraUp, cameraDir));

        updateWorldToView();
    }
}

gfsdk_float4x4 FPSCamera::perspective(const float fovy, const float aspect, const float zNear, const float zFar)
{
	gfsdk_float4x4 result = identity();
	float tanHalfFovy = tan(fovy / 2.0f);
	result._11 = 1.0f / (aspect * tanHalfFovy);
	result._22 = 1.0f / (tanHalfFovy);
	result._33 = zFar / (zFar - zNear);
	result._34 = 1.0f;
	result._43 = -(zFar * zNear) / (zFar - zNear);
	result._44 = 0;
	return result;
}

gfsdk_float4x4 FPSCamera::lookAtLH(const gfsdk_float3& eyePos, const gfsdk_float3& targetPoint, const gfsdk_float3& eyeUp)
{
	gfsdk_float3 forward = normalize(targetPoint - eyePos);
	gfsdk_float3 right = normalize(cross(eyeUp, forward));
	gfsdk_float3 up = cross(forward, right);
	gfsdk_float4x4 result = identity();
	result._11 = right.x;
	result._21 = right.y;
	result._31 = right.z;
	result._12 = up.x;
	result._22 = up.y;
	result._32 = up.z;
	result._13 = forward.x;
	result._23 = forward.y;
	result._33 = forward.z;
	result._41 = -dot(right, eyePos);
	result._42 = -dot(up, eyePos);
	result._43 = -dot(forward, eyePos);
	return result;
}

gfsdk_float4x4 FPSCamera::rotate(const float rotAngle, const gfsdk_float3& rotAxis)
{
	float cosA = cos(rotAngle);
	float sinA = sin(rotAngle);
	gfsdk_float3 axis = normalize(rotAxis);
	float temp = (1.0f - cosA);
	gfsdk_float4x4 result = identity();
	result._11 = temp*axis.x*axis.x + cosA;
	result._12 = temp*axis.x*axis.y - axis.z*sinA;
	result._13 = temp*axis.x*axis.z + axis.y*sinA;
	result._21 = temp*axis.x*axis.y + axis.z*sinA;
	result._22 = temp*axis.y*axis.y + cosA;
	result._23 = temp*axis.y*axis.z - axis.x*sinA;
	result._31 = temp*axis.x*axis.z - axis.y*sinA;
	result._32 = temp*axis.y*axis.z + axis.x*sinA;
	result._33 = temp*axis.z*axis.z + cosA;
	return result;
}
