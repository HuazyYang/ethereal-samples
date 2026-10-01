#include "DDGIVolume.h"

namespace ddgi {

int Sign(int s) { return s >= 0 ? 1 : -1; }

int Sign(float s) { return s >= 0.f ? 1 : -1; }

int AbsFloor(float s) { return s >= 0.f ? (int)std::floor(s) : (int)std::ceil(s); }

dm::quat RotationMatrixToQuaternion(const float3x3 &m) {
    dm::quat q = dm::quat::identity();

    float m00 = m.row0.x, m01 = m.row0.y, m02 = m.row0.z;
    float m10 = m.row1.x, m11 = m.row1.y, m12 = m.row1.z;
    float m20 = m.row2.x, m21 = m.row2.y, m22 = m.row2.z;
    float diagSum = m00 + m11 + m22;

    if (diagSum > 0.f) {
        q.w = sqrtf(diagSum + 1.f) * 0.5f;
        float f = 0.25f / q.w;
        q.x = (m21 - m12) * f;
        q.y = (m02 - m20) * f;
        q.z = (m10 - m01) * f;
    } else if ((m00 > m11) && (m00 > m22)) {
        q.x = sqrtf(m00 - m11 - m22 + 1.f) * 0.5f;
        float f = 0.25f / q.x;
        q.y = (m10 + m01) * f;
        q.z = (m02 + m20) * f;
        q.w = (m21 - m12) * f;
    } else if (m11 > m22) {
        q.y = sqrtf(m11 - m00 - m22 + 1.f) * 0.5f;
        float f = 0.25f / q.y;
        q.x = (m10 + m01) * f;
        q.z = (m21 + m12) * f;
        q.w = (m02 - m20) * f;
    } else {
        q.z = sqrtf(m22 - m00 - m11 + 1.f) * 0.5f;
        float f = 0.25f / q.z;
        q.x = (m02 + m20) * f;
        q.y = (m21 + m12) * f;
        q.w = (m10 - m01) * f;
    }

    return q;
}

void GetDDGIVolumeProbeCounts(const DDGIVolumeDesc& desc, uint32_t& probeCountX,
                                    uint32_t& probeCountY, uint32_t& probeCountZ) {
    probeCountX = desc.probeCounts.x;
    probeCountY = desc.probeCounts.y;
    probeCountZ = desc.probeCounts.z;
}

void GetDDGIVolumeTextureDimensions(const DDGIVolumeDesc& desc,
                                          EDDGIVolumeTextureType type, uint32_t& width,
                                          uint32_t& height, uint32_t& arraySize) {
    GetDDGIVolumeProbeCounts(desc, width, height, arraySize);
    switch (type) {
        case EDDGIVolumeTextureType::RayData:
            height = width * height;
            width = desc.probeNumRays;
            break;
        case EDDGIVolumeTextureType::Irradiance:
            width *= desc.probeNumIrradianceTexels;
            height *= desc.probeNumIrradianceTexels;
            break;
        case EDDGIVolumeTextureType::Distance:
            width *= desc.probeNumDistanceTexels;
            height *= desc.probeNumDistanceTexels;
            break;
        case EDDGIVolumeTextureType::Variability:
            width *= desc.probeNumIrradianceInteriorTexels;
            height *= desc.probeNumIrradianceInteriorTexels;
            break;
        case EDDGIVolumeTextureType::VariabilityAverage: {
            // Start with Probe variability texture dimensions
            width *= (uint32_t)(desc.probeNumIrradianceInteriorTexels);
            height *= (uint32_t)(desc.probeNumIrradianceInteriorTexels);
            // Divide into thread groups, should match NUM_THREADS_XYZ in ReductionCS.hlsl
            const uint3 NumThreadsInGroup = {4, 8, 4};
            // Also divide by sample footprint per-thread, should match
            // ThreadSampleFootprint in ReductionCS.hlsl
            const uint3 DimensionScale = {NumThreadsInGroup.x * 4, NumThreadsInGroup.y * 2,
                                          NumThreadsInGroup.z};
            // Size of diff total texture is just diff divided by thread group dimensions,
            // rounded up
            width = (width + DimensionScale.x - 1) / DimensionScale.x;
            height = (height + DimensionScale.y - 1) / DimensionScale.y;
            arraySize = (arraySize + DimensionScale.z - 1) / DimensionScale.z;
        }
        break;
    }
}

DDGIVolume::DDGIVolume(const DDGIVolumeDesc &desc) {
    m_desc = desc;
    m_rotationQuaternion = dm::rotationQuat(m_desc.eulerAngles);
    m_rotationMatrix = m_rotationQuaternion.toMatrix();
    m_probeScrollAnchor = m_desc.origin;

    // Initialize the random number generator if a seed is provided, otherwise use the
    // default std::random_device()
    if (desc.rngSeed != 0) {
        SeedRNG(desc.rngSeed);
    } else {
        std::random_device rd;
        SeedRNG(rd());
    }
}

DDGIVolumeDescGPU DDGIVolume::GetDescGPU() const {
    DDGIVolumeDescGPU descGPU = {};
    descGPU.origin = m_desc.origin;
    descGPU.rotation = m_rotationQuaternion.toXYZW();
    descGPU.probeRayRotation = m_probeRayRotationQuaternion.toXYZW();
    descGPU.movementType = static_cast<uint32_t>(m_desc.movementType);
    descGPU.probeSpacing = m_desc.probeSpacing;
    descGPU.probeCounts = m_desc.probeCounts;
    descGPU.probeNumRays = m_desc.probeNumRays;
    descGPU.probeNumIrradianceInteriorTexels = m_desc.probeNumIrradianceInteriorTexels;
    descGPU.probeNumDistanceInteriorTexels = m_desc.probeNumDistanceInteriorTexels;
    descGPU.probeHysteresis = m_desc.probeHysteresis;
    descGPU.probeMaxRayDistance = m_desc.probeMaxRayDistance;
    descGPU.probeNormalBias = m_desc.probeNormalBias;
    descGPU.probeViewBias = m_desc.probeViewBias;
    descGPU.probeDistanceExponent = m_desc.probeDistanceExponent;

    descGPU.probeIrradianceEncodingGamma = m_desc.probeIrradianceEncodingGamma;
    descGPU.probeIrradianceThreshold = m_desc.probeIrradianceThreshold;
    descGPU.probeBrightnessThreshold = m_desc.probeBrightnessThreshold;

    descGPU.probeRandomRayBackfaceThreshold =
        std::clamp(m_desc.probeRandomRayBackfaceThreshold, 0.f, 1.f);
    descGPU.probeFixedRayBackfaceThreshold =
        std::clamp(m_desc.probeFixedRayBackfaceThreshold, 0.f, 1.f);

    descGPU.probeMinFrontfaceDistance = m_desc.probeMinFrontfaceDistance;

    // 15-bits used for scroll offsets (plus 1 sign bit), maximum magnitude of 32,767
    descGPU.probeScrollOffsets.x =
        std::min(32767, abs(m_probeScrollOffsets.x)) * Sign(m_probeScrollOffsets.x);
    descGPU.probeScrollOffsets.y =
        std::min(32767, abs(m_probeScrollOffsets.y)) * Sign(m_probeScrollOffsets.y);
    descGPU.probeScrollOffsets.z =
        std::min(32767, abs(m_probeScrollOffsets.z)) * Sign(m_probeScrollOffsets.z);

    descGPU.probeRayDataFormat = static_cast<uint32_t>(m_desc.probeRayDataFormat);
    descGPU.probeIrradianceFormat = static_cast<uint32_t>(m_desc.probeIrradianceFormat);
    descGPU.probeRelocationEnabled = m_desc.probeRelocationEnabled;
    descGPU.probeClassificationEnabled = m_desc.probeClassificationEnabled;
    descGPU.probeVariabilityEnabled = m_desc.probeVariabilityEnabled;
    descGPU.probeScrollClear[0] = m_probeScrollClear[0];
    descGPU.probeScrollClear[1] = m_probeScrollClear[1];
    descGPU.probeScrollClear[2] = m_probeScrollClear[2];
    descGPU.probeScrollDirections[0] = (m_probeScrollDirections[0] > 0);
    descGPU.probeScrollDirections[1] = (m_probeScrollDirections[1] > 0);
    descGPU.probeScrollDirections[2] = (m_probeScrollDirections[2] > 0);

    return descGPU;
}

DDGIVolumeDescGPUPacked DDGIVolume::GetDescGPUPacked() const {
    return PackDDGIVolumeDescGPU(GetDescGPU());
}

void DDGIVolume::Update() {
    // Update the random probe ray rotation transform
    ComputeRandomRotation();

    // Update scrolling offsets and clear flags
    if (m_desc.movementType == EDDGIVolumeMovementType::Scrolling) ComputeScrolling();
}

float3 DDGIVolume::GetOrigin() const {
    if (m_desc.movementType == EDDGIVolumeMovementType::Default) return m_desc.origin;

    return {m_desc.origin.x + ((float)m_probeScrollOffsets.x * m_desc.probeSpacing.x),
            m_desc.origin.y + ((float)m_probeScrollOffsets.y * m_desc.probeSpacing.y),
            m_desc.origin.z + ((float)m_probeScrollOffsets.z * m_desc.probeSpacing.z)};
}

void DDGIVolume::GetRayDispatchDimensions(uint32_t& width, uint32_t& height,
                                          uint32_t& depth) const {
    GetDDGIVolumeTextureDimensions(m_desc, EDDGIVolumeTextureType::RayData, width, height,
                                   depth);
}

void DDGIVolume::SeedRNG(int seed) { m_rng.seed(seed); }

float DDGIVolume::GetRandomFloat() { return m_udf(m_rng); }

void DDGIVolume::ComputeRandomRotation() {
    // This approach is based on James Arvo's implementation from Graphics Gems 3 (pg
    // 117-120).
    // Also available at:
    // http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.53.1357&rep=rep1&type=pdf

    // Setup a random rotation matrix using 3 uniform RVs
    float u1 = 2.f * PI_f * GetRandomFloat();
    float cos1 = cosf(u1);
    float sin1 = sinf(u1);

    float u2 = 2.f * PI_f * GetRandomFloat();
    float cos2 = cosf(u2);
    float sin2 = sinf(u2);

    float u3 = GetRandomFloat();
    float sq3 = 2.f * sqrtf(u3 * (1.f - u3));

    float s2 = 2.f * u3 * sin2 * sin2 - 1.f;
    float c2 = 2.f * u3 * cos2 * cos2 - 1.f;
    float sc = 2.f * u3 * sin2 * cos2;

    // Create the random rotation matrix
    float _11 = cos1 * c2 - sin1 * sc;
    float _12 = sin1 * c2 + cos1 * sc;
    float _13 = sq3 * cos2;

    float _21 = cos1 * sc - sin1 * s2;
    float _22 = sin1 * sc + cos1 * s2;
    float _23 = sq3 * sin2;

    float _31 = cos1 * (sq3 * cos2) - sin1 * (sq3 * sin2);
    float _32 = sin1 * (sq3 * cos2) + cos1 * (sq3 * sin2);
    float _33 = 1.f - 2.f * u3;

    float3x3 transform;
    transform.row0 = {_11, _12, _13};
    transform.row1 = {_21, _22, _23};
    transform.row2 = {_31, _32, _33};

    m_probeRayRotationMatrix = transform;
    m_probeRayRotationQuaternion = RotationMatrixToQuaternion(transform);
}

void DDGIVolume::ScrollReset() {
    // Reset the volume's origin and scroll offsets (if necessary) for each axis
    for (int planeIndex = 0; planeIndex < 3; planeIndex++) {
        if (m_probeScrollOffsets[planeIndex] != 0 &&
            (m_probeScrollOffsets[planeIndex] % m_desc.probeCounts[planeIndex] == 0)) {
            m_desc.origin[planeIndex] +=
                ((float)m_desc.probeCounts[planeIndex] * m_desc.probeSpacing[planeIndex]) *
                (float)m_probeScrollDirections[planeIndex];
            m_probeScrollOffsets[planeIndex] = 0;
        }
    }
}

void DDGIVolume::ComputeScrolling() {
    // Reset plane clear flags
    m_probeScrollClear[0] = false;
    m_probeScrollClear[1] = false;
    m_probeScrollClear[2] = false;

    // Reset scroll offsets to not overflow (eventually)
    ScrollReset();

    // Get the world-space translation and direction between the (effective) origin and the
    // scroll anchor
    float3 translation = m_probeScrollAnchor - GetOrigin();
    m_probeScrollDirections = {Sign(translation.x), Sign(translation.y),
                               Sign(translation.z)};

    // Get the number of grid cells between the (effective) origin and the scroll anchor
    int3 scroll = {
        AbsFloor(translation.x / m_desc.probeSpacing.x),
        AbsFloor(translation.y / m_desc.probeSpacing.y),
        AbsFloor(translation.z / m_desc.probeSpacing.z),
    };

    if (scroll.x != 0) {
        m_probeScrollOffsets.x += scroll.x;
        m_probeScrollClear[0] = true;
    }

    if (scroll.y != 0) {
        m_probeScrollOffsets.y += scroll.y;
        m_probeScrollClear[1] = true;
    }

    if (scroll.z != 0) {
        m_probeScrollOffsets.z += scroll.z;
        m_probeScrollClear[2] = true;
    }
}

int3 DDGIVolume::GetProbeGridCoords(int probeIndex) const {
    int x = probeIndex % m_desc.probeCounts.x;
    int y = probeIndex / (m_desc.probeCounts.x * m_desc.probeCounts.z);
    int z = (probeIndex / m_desc.probeCounts.x) % m_desc.probeCounts.z;
    return {x, y, z};
}

}  // namespace ddgi