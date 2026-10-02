#ifndef GVDBCAMERA_H
#define GVDBCAMERA_H
#include <nvrhi/core/foundation.h>
#include <donut/core/math/math.h>
#include <cmath>

namespace SampleUtils {

// TODO(migration): donut's dm::rotationQuat(axis, radians) cannot be instantiated -
// its body calls quaternion<T>(w, vector<T,3>), a constructor that only existed in the
// old ethereal fork. This local helper expresses the identical rotation using donut's
// public dm::quat::fromWXYZ() factory.
inline dm::quat axisAngleQuat(const dm::float3 &axis, float radians) {
    const float sinHalf = std::sin(0.5f * radians);
    return dm::quat::fromWXYZ(std::cos(0.5f * radians), axis * sinHalf);
}

class Camera: public nvrhi::ObjectImpl<nvrhi::IObject>  {
public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Camera)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IObject)
    NVRHI_END_INTERFACE_TABLE()

   /**
    * @brief specify the rotation convention of WCS, VCS, HCS
    * @p rightHandled
    *   - true WCS, VCS, HCS will be defined as right-handed CS
    *   - false WCS, VCS, HCS is left-handed CS
    */
   void setProjectionRH(bool rightHanded) {
       m_isProjectionRH = rightHanded;
       m_flushViewMatrix = true;
       m_flushProjMatrix = true;
   }

   void setViewParamsSpherical(dm::float3 target, float radius, float theta, float phi) {
       m_target = target;
       m_radius = radius;

       //  right-handed to left-handed
       if (m_isProjectionRH) {
           m_target.z = -m_target.z;
           phi = dm::PI_f - phi;
       }

       m_pan = axisAngleQuat(dm::float3{0.f, 1.f, 0.f}, dm::PI_f - phi);
       m_tilt = axisAngleQuat(dm::float3{1.f, 0.f, 0.f},
                              theta - dm::PI_f * 0.5f);
       m_rotation = m_tilt * m_pan;
       m_flushViewMatrix = 1;
   }

   void setViewParams(dm::float3 target, dm::float3 position) {
       m_target = target;
       dm::float3 fwd = target - position;

       if (m_isProjectionRH) {
           m_target.z = -m_target.z;
           fwd.z = -fwd.z;
       }

       m_radius = dm::length(fwd);
       fwd /= m_radius;
       float sinTheta = std::sqrt(fwd.z * fwd.z + fwd.x * fwd.x);
       float cosPhi = fwd.z / sinTheta;
       float sinPhi = fwd.x / sinTheta;
       float cosTheta = fwd.y;
       // -phi
       float sinHalfPhiP = std::max(0.f, std::sqrt(0.5f - 0.5f * cosPhi));
       float cosHalfPhiP = -sinPhi / (2.f * sinHalfPhiP);
       m_pan = dm::quat::fromWXYZ(cosHalfPhiP, dm::float3{0.f, sinHalfPhiP, 0.f});
       // pi/2 - theta
       float sinHalfThetaP = std::max(0.f, std::sqrt(0.5f - 0.5f * sinTheta));
       float cosHalfThetaP = cosTheta / (2.f * sinHalfThetaP);
       m_tilt = dm::quat::fromWXYZ(cosHalfThetaP, dm::float3{sinHalfThetaP, 0.f, 0.f});

       m_rotation = m_tilt * m_pan;
       m_flushViewMatrix = 1;
    }

    void setWindowParams(int winLeft, int winTop, int winWidth, int winHeight) {
        m_windowLeft = winLeft;
        m_windowTop = winTop;
        m_windowWidth = winWidth;
        m_windowHeight = winHeight;
        m_flushProjMatrix = 1;
    }

    void setProjectParams(float fovY, float zNear, float zFar) {
        m_fovY = fovY;
        m_zNear = zNear;
        m_zFar = zFar;
        m_flushProjMatrix = 1;
    }

    void rotationStart(int x, int y) {
        m_windowCursorPos = {x, y};
        m_rotationActive = 1;
    }

    void rotationMove(int x, int y) {
        if(m_rotationActive) {
            int dx = -(x - m_windowCursorPos.x);
            int dy = -(y - m_windowCursorPos.y);

            const float gainX = 2.f * dm::PI_f / m_windowWidth;
            const float gainY = dm::PI_f / m_windowHeight;

            float rotx = gainY * dy;
            float roty = gainX * dx;
            auto tilt = axisAngleQuat(dm::float3{1.f, 0.f, 0.f}, rotx) * m_tilt;
            auto pan = axisAngleQuat(dm::float3{0.f, 1.f, 0.f}, roty) * m_pan;

            m_tilt = tilt;
            m_pan = pan;

            m_rotation = m_tilt * m_pan;

            m_windowCursorPos = {x, y};
            m_flushViewMatrix = 1;
        }
    }

    void rotationEnd(int x, int y) {
        m_windowCursorPos = {x, y};
        m_rotationActive = 0;
    }

    void zoomStart(int x, int y) {
        m_windowCursorPos = {x, y};
        m_zoomActive = 1;
    }

    void zoomMove(int x, int y) {
        if(m_zoomActive) {
            float dx = -float(x - m_windowCursorPos.x);
            float dy = -float(y - m_windowCursorPos.y);

            const float gain = dm::PI_f / float(m_windowHeight);
            m_radius *= (1.f + gain * dy);

            m_windowCursorPos = {x, y};
            m_flushViewMatrix = 1;
        }
    }

    void zoomEnd(int x, int y) {
        if(m_zoomActive) {
            m_windowCursorPos = {x, y};
            m_zoomActive = 0;
        }
    }

    void translationStart(int x, int y) {
        m_windowCursorPos = {x, y};
        m_translateActive = 1;
    }

    void translationMove(int x, int y) {
        if(m_translateActive) {
            float dx = float(x - m_windowCursorPos.x);
            float dy = -float(y - m_windowCursorPos.y);

            const float gainX = (m_isProjectionRH ? -2.f : 2.f) / m_windowWidth;
            const float gainY = (m_isProjectionRH ? -2.f : 2.f) / m_windowHeight;

            // find rotation center in screen space
            dm::float4 centerScreen = dm::float4(0.f, 0.f, m_radius, 1.f) * m_projMatrix;

            dm::float4x4 viewProjInv = dm::inverse(dm::affineToHomogeneous(m_rotation.toAffine() * dm::translation(dm::float3{0.f, 0.f, m_radius})) * m_projMatrix);

            dm::float4 offsetScreen{gainX * dx, gainY * dy, centerScreen.z / centerScreen.w, 1.f};
            dm::float4 offsetWorld = offsetScreen * viewProjInv;
            offsetWorld /= offsetWorld.w;

            m_target -= offsetWorld.xyz();
            m_windowCursorPos =  {x, y};
            m_flushViewMatrix = 1;
        }
    }

    void translationEnd(int x, int y) {
        m_windowCursorPos = {x, y};
        m_translateActive = 0;
    }

    void update() {
        if(m_flushViewMatrix)
            updateViewMatrix();
        if(m_flushProjMatrix)
            updateProjMatrix();
    }

    const dm::float4x4& getViewMatrix() const {
        return m_viewMatrix;
    }

    const dm::float4x4& getProjMatrix() const {
        return m_projMatrix;
    }

    dm::float3 getCameraPos() const { return m_viewMatrixInv[3].xyz(); }

    const dm::float4x4& getViewMatrixInv() const {
        return m_viewMatrixInv;
    }

    float getZNear() const { return m_zNear; }

    float getZFar() const { return m_zFar; }

    dm::float3 getCameraPos() { return m_viewMatrixInv[3]; }

 private:
    void updateViewMatrix() {
        auto viewMat = dm::translation(-m_target) * m_rotation.toAffine() * dm::translation(dm::float3{0.f, 0.f, m_radius});
        if(m_isProjectionRH) {
            viewMat = dm::scaling(dm::float3{1.f, 1.f, -1.f}) * viewMat *
                dm::scaling(dm::float3{1.f, 1.f, -1.f});
        }
        m_viewMatrix = dm::affineToHomogeneous(viewMat);

        m_viewMatrixInv = dm::inverse(m_viewMatrix);

        m_flushViewMatrix = 0;
    }

    void updateProjMatrix() {
        if(m_isProjectionRH) {
            // TODO(migration): dropped - dm::perspectiveFovRH() exists in the old ethereal
            // fork but not in donut. donut's nearest function, dm::perspProjOGLStyle(), is
            // NOT equivalent (it maps depth to [-1,1] instead of [0,1]), so the
            // right-handed projection path is dropped rather than silently substituted.
            // m_projMatrix = dm::perspectiveFovRH(m_fovY, float(m_windowWidth) / m_windowHeight, m_zNear, m_zFar);
            NVRHI_ASSERT(0 && "right-handed projection dropped during ethereal->donut migration");
            m_projMatrix = dm::float4x4::identity();
        } else {
            // dm::perspProjD3DStyle() is bit-for-bit the old fork's dm::perspectiveFovLH()
            m_projMatrix = dm::perspProjD3DStyle(m_fovY, float(m_windowWidth) / m_windowHeight, m_zNear, m_zFar);
        }

        m_flushProjMatrix = 0;
    }

    dm::int2 mapWindowPos(int x, int y) {
        return dm::int2{x - m_windowLeft, y - m_windowTop};
    }

    dm::float3 m_target = dm::float3::zero();
    float m_radius = 1.f;
    dm::quat m_pan = dm::quat::identity();
    dm::quat m_tilt = dm::quat::identity();
    dm::quat m_rotation = dm::quat::identity();

    float m_fovY = dm::PI_f / 4.f;
    float m_zNear = 0.1f, m_zFar = 1000.f;

    int m_windowLeft = 0, m_windowTop = 0;
    int m_windowWidth = 1, m_windowHeight = 1;

    dm::float4x4 m_viewMatrix = dm::float4x4::identity();
    dm::float4x4 m_viewMatrixInv = dm::float4x4::identity();
    dm::float4x4 m_projMatrix = dm::float4x4::identity();

    uint32_t m_isProjectionRH = 0;

    // Interaction state
    dm::int2 m_windowCursorPos = {0, 0};
    uint32_t m_rotationActive = 0;
    uint32_t m_zoomActive = 0;
    uint32_t m_translateActive = 0;

    // Internal state
    uint32_t m_flushViewMatrix = 1;
    uint32_t m_flushProjMatrix = 1;
};

class Light : public Camera {
    NVRHI_INHERIT_INTERFACE_TABLE()
};

}  // namespace SampleUtils

#endif /* GVDBCAMERA_H */
