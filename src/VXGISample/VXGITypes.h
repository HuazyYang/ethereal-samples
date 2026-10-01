#ifndef SRC_VXGISAMPLE_VXGITYPES_H
#define SRC_VXGISAMPLE_VXGITYPES_H
#include <donut/core/math/math.h>
#include <nvrhi/core/Types.h>
#include <nvrhi/nvrhi.h>

namespace nvrhi {
struct IGraphicsPipeline;
struct ITexture;
}

namespace vxgi {

using namespace dm;

enum class Status: nvrhi::FLONG {
    OK = 0,
    WRONG_INTERFACE_VERSION = 1,

    D3D_COMPILER_UNAVAILABLE,
    INSUFFICIENT_BINDING_SLOTS,
    INTERNAL_ERROR,
    INVALID_ARGUMENT,
    INVALID_CONFIGURATION,
    INVALID_SHADER_BINARY,
    INVALID_SHADER_SOURCE,
    INVALID_STATE,
    NULL_ARGUMENT,
    RESOURCE_CREATION_FAILED,
    SHADER_COMPILATION_ERROR,
    SHADER_MISSING,
    FUNCTION_MISSING,
    BUFFER_TOO_SMALL,
    NOT_SUPPORTED
};

enum class OpacityDirections {
    THREE_DIMENSIONAL = 3,
    SIX_DIMENSIONAL = 6,
};

enum class EmittanceFormat {
    NONE = 0,         // no emittance - ambient occlusion mode
    PERFORMANCE = 1,  // use FLOAT16 if it is supported, UNORM8 otherwise
    QUALITY = 2,      // use FLOAT16 if it is supported, FLOAT32 otherwise
    UNORM8 = 3,       // use RGBA8_UNORM_SRGB - lowest quality mode
    FLOAT16 = 4,      // use RGBA16_FLOAT; only supported on Maxwell [*] GPUs when
                      // enableNvidiaExtensions == true
    FLOAT32 = 5       // use 3x R32_FLOAT textures
};

enum class DebugRenderMode {
    DISABLED = 0,
    ALLOCATION_MAP,
    OPACITY_TEXTURE,
    EMITTANCE_TEXTURE,
    INDIRECT_IRRADIANCE_TEXTURE
};

enum VoxelSizeFunction {
        EXACT = 0,
        LINEAR_UNDERESTIMATE = 1,
        LINEAR_OVERESTIMATE = 2
};

struct CommonTracingParameters {
    // Maximum number of samples that can be fetched for each cone
    uint32_t maxSamples = 128;

    // Tracing step.
    // Reasonable values [0.5, 1]
    // Sampling with lower step produces more stable results at Performance cost.
    float tracingStep = 1.0f;

    // Opacity correction factor.
    // Reasonable values [0.1, 10]
    // Higher values produce more contrast rendering, overall picture looks darker.
    float opacityCorrectionFactor = 1.0f;

    // Multiplier for the incoming light intensity.
    float irradianceScale = 1.0f;

    // Flips the direction in which geometry blocks light - sometimes it helps improve
    // occlusion quality, for example it reduces "peter panning" of objects in specular
    // reflections.
    bool flipOpacityDirections = false;

    // These should be set to zero and normally have no effect
    dm::float4 debugParameters = 0.f;

    // Projection parameters: near and far clip planes' post-projection Z values
    float nearClipZ = 0.f;  // 0.0 for regular projections
    float farClipZ = 1.f;   // 1.0 for regular projections
};

struct DiffuseTracingParameters : public CommonTracingParameters {
    // Number of diffuse cones to trace for each fragment, 4 or more.
    // Balances Quality (more cones) vs Performance
    uint32_t numCones = 8;

    // Automatic diffuse angle computation based on the number of cones
    // Overrides the value set in coneAngle.
    // Use IViewTracer::getDiffuseConeAngle(numCones) to get the actual diffuse angle.
    bool autoConeAngle = true;

    // Cone angle for GI diffuse component evaluation, in degrees
    // This value has no effect if autoConeAngle == true
    float coneAngle = 60.f;

    // Optional color for adding occluded ambient lighting.
    // To get a normalized ambient occlusion only rendering, set ambientColor to 1.0 and
    // irradianceScale to 0.
    dm::float3 ambientColor = 0.f;

    // World-space distance at which the contribution of geometry to ambient occlusion will
    // be 10x smaller than near the surface.
    float ambientRange = 512.f;

    // Other parameters for the ambient term.
    // correctedConeAmbient = pow(saturate(cone.ambient * ambientScale + ambientBias,
    // ambientPower))
    float ambientScale = 1.f;
    float ambientBias = 0.f;
    float ambientPower = 1.f;

    // Parameter that controls how much darker to make ambient occlusion at distance. [0..1]
    // With ambientDistanceDarkening == 0, occlusion tends to disappear close to clip-map
    // edges because of large initial offsets.
    float ambientDistanceDarkening = 0.25f;

    // Diffuse tracing results can be alpha-blended over this color, where alpha is indirect
    // / AO confidence. If this color is black, diffuse RGB are not pre-multiplied by alpha.
    dm::float3 backgroundColor = 0.f;

    // Environment map to use when diffuse cones don't hit any geometry. Optional.
    nvrhi::ITexture* environmentMap = nullptr;

    // Multiplier for the environment map colors.
    // The environment map samples are multiplied by cones' final transmittance values.
    // In ambient occlusion mode, i.e. when VoxelizationParameters::emittanceFormat == NONE,
    // the environment map is ignored.
    dm::float3 environmentMapTint = 0.f;

    // Random per-pixel rotation of the diffuse cone set - it helps reduce banding but costs
    // some performance
    bool enableConeRotation = false;

    // Random per-pixel adjustment of initial tracing offsets for diffuse tracing, also
    // helps reduce banding
    bool enableRandomConeOffsets = false;

    // A factor that controls linear interpolation between smoothNormal and ray direction.
    // The interpolated value is used to direct the initial cone offset.
    // Accepted values are [0, 1]
    float normalOffsetFactor = 0.5f;

    // Diffuse tracing sparsity.
    // 1 = dense tracing (for every pixel),
    // 2..4 = sparse tracing (for one pixel in every NxN square).
    // Using sparse tracing greatly improves performance in exchange for fine detail
    // quality.
    uint32_t tracingSparsity = 2;

    // Bigger factor would move the diffuse cones closer to the surface normal
    // Reasonable values [0, 1]
    float coneNormalGroupingFactor = 0.f;

    // Parameters that control the distance of the first sample from the surface
    float initialOffsetBias = 2.f;
    float initialOffsetDistanceFactor = 1.f;

    // Enables reuse of diffuse tracing results from the previous frame.
    // For this mode to work, different G-buffer textures have to be used for consecutive
    // frames, at least the depth and normal channels. Otherwise the tracing calls will
    // fail.
    bool enableTemporalReprojection = false;

    // Weight of the reprojected irradiance data relative to newly computed data, (0..1),
    // where 0 means do not use reprojection, and values closer to 1 mean accumulate data
    // over more previous frames. Higher values result in better filtering of random
    // rotation/offset noise, but also introduce a lag of GI from moving objects. 1.0 or
    // higher will produce an unstable result.
    float temporalReprojectionWeight = 0.9f;

    // Maximum distance between two samples for which they're still considered to be the
    // same surface, expressed in voxels.
    float temporalReprojectionMaxDistanceInVoxels = 0.25f;

    // The exponent used for the dot product of old and new normals in the temporal
    // reprojection filter
    float temporalReprojectionNormalWeightExponent = 20.f;

    // Normally, the computeDiffuseChannel method will test that the previous-frame input
    // buffers are different from the current-frame ones, and disable reprojection if they
    // are the same. Setting enableReprojectionFromSameFrame == true removes this test.
    bool enableReprojectionFromSameFrame = false;

    // Enables a second tracing pass, performed only on pixels that do not have enough
    // information from the sparse tracing pass
    bool enableSparseTracingRefinement = true;

    // Minimum pixel weight for sparse tracing interpolation. When a pixel is below that
    // weight, it is considered to be a hole and submitted for refinement. When refinement
    // is disabled, such pixels will be black with zero confidence. Using a higher threshold
    // improves image quality and reduces temporal noise at the cost of refinement pass
    // performance. Clamped to [0, 1].
    float interpolationWeightThreshold = 1e-4f;

    // Versions of the settings for objects marked in the stencil buffer.
    // See IViewTracer::InputBuffers::altSettingsStencilMask and altSettingsStencilRefValue
    // for more info.
    float altInitialOffsetBias = 2.f;
    float altInitialOffsetDistanceFactor = 1.f;
    float altNormalOffsetFactor = 0.5f;
    float altTracingStep = 0.5f;

    // Enables a built-in SSAO pass that is multiplied into the diffuse tracing results.
    // The SSAO algorithm used here and its parameters are very similar to HBAO+.
    bool enableSSAO = false;
    float SSAO_SurfaceBias = 0.2f;
    float SSAO_RadiusWorld = 50.f;
    float SSAO_BackgroundViewDepth = 1000.f;
    float SSAO_Scale = 1.f;
    float SSAO_PowerExponent = 2.f;

    DiffuseTracingParameters() {
        // Override the default step of 1.0
        tracingStep = 0.5f;
    }
};

struct SpecularTracingParameters : public CommonTracingParameters {
    enum Filter {
        FILTER_NONE,
        FILTER_TEMPORAL,
        FILTER_SIMPLE
    };

    // Selects the filter that is used on the specular surface after tracing in order to
    // reduce noise introduced by cone jitter.
    Filter filter = FILTER_SIMPLE;

    // Parameters that control the distance of the first specular cone sample from the
    // surface
    float initialOffsetBias = 2.f;
    float initialOffsetDistanceFactor = 1.f;

    // Environment map to use when specular cones don't hit any geometry. Optional.
    nvrhi::ITexture *environmentMap = nullptr;

    // Multiplier for environment map reflections in the specular channel.
    // The environment map will only be visible on pixels that do not reflect any solid
    // geometry.
    dm::float3 environmentMapTint = 0.f;

    // Weight of the reprojected irradiance data relative to newly computed data, [0..1).
    // Only applicable if filter == FILTER_TEMPORAL.
    // Also see the comment for DiffuseTracingParameters::enableTemporalReprojection and
    // temporalReprojectionWeight.
    float temporalReprojectionWeight = 0.8f;

    // Maximum distance between two samples for which they're still considered to be the
    // same surface, expressed in voxels.
    float temporalReprojectionMaxDistanceInVoxels = 0.25f;

    // The exponent used for the dot product of old and new normals in the temporal
    // reprojection filter
    float temporalReprojectionNormalWeightExponent = 20.f;

    // Normally, the computeSpecularChannel method will test that the previous-frame input
    // buffers are different from the current-frame ones, and disable reprojection if they
    // are the same. Setting enableReprojectionFromSameFrame == true removes this test.
    bool enableReprojectionFromSameFrame = false;

    // Scale of the jitter can be added to specular sample positions to reduce blockiness of
    // the reflections, [0..1] When filter == FILTER_TEMPORAL, the noise pattern is
    // different on every frame.
    float tangentJitterScale = 0.f;
};

struct TracerVisionParameters : public CommonTracingParameters {
    // Cone angle for the Tracer Vision debug mode
    float coneAngle = 1.f;
};

struct IndirectIrradianceMapTracingParameters : public CommonTracingParameters {
    float coneAngle = 40.f;

    // Auto-normalization is a safeguard algorithm that attempts to prevent irradiance from
    // blowing up. In some cases, such as when the value of irradianceScale in this
    // structure is too large, or when multiple surfaces with the same orientation, one
    // behind another, receive indirect illumination, multibounce tracing feedback factor
    // may become greater than 1, which means irradiance will blow up exponentially, causing
    // numeric overflows in the voxel textures (or saturation in case UNORM8 emittance is
    // used). The normalization algorithm analyzes average irradiance over multiple frames,
    // and adjusts irradianceScale when it detects that an exponential blow-up is happening.
    // Adjustment is done internally, not visible to the user, and the adjustment factor is
    // reset to 1.0 when the value of irradianceScale in this structure is different from
    // one used on the previous frame.
    bool useAutoNormalization = true;

    // Hard limit for indirect irradiance values. Useful in cases when auto-normalization
    // doesn't work properly. When irradianceClampValue <= 0 or >= 65504.0, the internal
    // limit is set to 65504.0 because that's the maximum value representable as float16
    // that's not infinity.
    float irradianceClampValue = 0.f;
};

struct VoxelizationParameters {
    // Controls voxelization density, must be a power of 2 and within range [16, 256]
    uint32_t mapSize = 64;

    // Controls allocation granularity, bigger values result in coarser allocation map being
    // used
    uint32_t allocationMapLodBias = 0;

    // Number of levels in a clipmap stack used for scene representation.
    // Reasonable values [2, log2(MapSize) - 1]
    uint32_t stackLevels = 5;

    // Number of levels in a mipmap stack used for scene representation.
    // These levels do not include stack levels, and there can be 0 of them.
    uint32_t mipLevels = 5;

    // Controls whether opacity and emittance voxel data can be preserved between frames.
    // If not, the whole clipmap will be automatically invalidated during the
    // prepareForOpacityVoxelization call, and some passes of the VXGI algorithm will become
    // faster.
    bool persistentVoxelData = true;

    // Enables a mode where VXGI does not optimize the invalidation regions before
    // processing them on the GPU. Updates to voxel data on the GPU will still happen with
    // one page granularity, but the application will receive just one "optimized" region
    // which is a union of all submitted regions, rounded to page size. The value of
    // simplifiedInvalidate does not matter if persistentVoxelData == false.
    bool simplifiedInvalidate = true;

    // The number of opacity directions stored per voxel.
    OpacityDirections opacityDirectionCount = OpacityDirections::SIX_DIMENSIONAL;

    // Controls whether the library should try to use NVIDIA-specific hardware features
    bool enableNvidiaExtensions = true;

    // Enables the use of Maxwell Geometry Shader Pass-Through feature for voxelization.
    // Only effective when enableNvidiaExtensions == true.
    // Sometimes pass-through shaders do not work properly (e.g. broken texture coordinates
    // in PS) while other Maxwell features do, so this flag allows applications to work
    // around the issues at a small performance cost.
    bool enableGeometryShaderPassthrough = true;

    // Controls the format of the textures used to store emittance.
    EmittanceFormat emittanceFormat = EmittanceFormat::PERFORMANCE;

    // Global multiplier for emittance voxels - adjust it according to your light
    // intensities to avoid clamping or quantization. As long as neither of these effects
    // takes place, changing this parameter has no effect on rendered images.
    float emittanceStorageScale = 1.f;

    // Enables a mode wherein transitions from downsampled to directly voxelized emittance
    // are smoothed by blending the two with a factor that depends on the exact clipmap
    // anchor, not the quantized clipmap center. This mode requires persistentVoxelData =
    // false to operate properly.
    bool useEmittanceInterpolation = false;

    // Enables a higher-order filter to be used during emittance downsampling.
    // Using this filter makes moving objects produce much smoother indirect illumination,
    // at a significant performance cost.
    bool useHighQualityEmittanceDownsampling = false;

    // Controls whether a separate indirect irradiance 3D map is computed for use on the
    // next frame. In order to see multi-bounce lighting, call this function in the
    // emittance voxelization pixel shader:
    //
    //    float3 VxgiGetIndirectIrradiance(float3 worldPos, float3 normal)
    //
    // Then shade the material with the returned indirect irradiance (don't forget to divide
    // it by PI) and add the result to output color.
    bool enableMultiBounce = false;

    // Controls the size of the indirect irradiance map.
    // - indirectIrradianceMapLodBias == 0 means that the indirect irradiance map will have
    // the same size and resolution
    //    as the coarsest clipmap level.
    // - indirectIrradianceMapLodBias > 0 means that there will be fewer voxels, but not
    // fewer than in the allocation map,
    //    i.e. indirectIrradianceMapLodBias <= allocationMapLodBias.
    // - indirectIrradianceMapLodBias < 0 means that there will be more voxels, but not more
    // than 256^3, and the resolution
    //    will not be finer than the finest clipmap level, i.e. indirectIrradianceMapLodBias
    //    > -stackLevels.
    int32_t indirectIrradianceMapLodBias = 0;

    bool operator!=(const VoxelizationParameters& parameters) const {
        if (parameters.mapSize != mapSize ||
            parameters.allocationMapLodBias != allocationMapLodBias ||
            parameters.stackLevels != stackLevels || parameters.mipLevels != mipLevels ||
            parameters.persistentVoxelData != persistentVoxelData ||
            parameters.simplifiedInvalidate != simplifiedInvalidate ||
            parameters.opacityDirectionCount != opacityDirectionCount ||
            parameters.enableNvidiaExtensions != enableNvidiaExtensions ||
            parameters.enableGeometryShaderPassthrough != enableGeometryShaderPassthrough ||
            parameters.emittanceFormat != emittanceFormat ||
            parameters.emittanceStorageScale != emittanceStorageScale ||
            parameters.useEmittanceInterpolation != useEmittanceInterpolation ||
            parameters.useHighQualityEmittanceDownsampling !=
                useHighQualityEmittanceDownsampling ||
            parameters.enableMultiBounce != enableMultiBounce ||
            parameters.indirectIrradianceMapLodBias != indirectIrradianceMapLodBias)
            return true;

        return false;
    }

    bool operator==(const VoxelizationParameters& parameters) const {
        return !(*this != parameters);
    }
};

struct TracedSamplesParameters {
    enum ColorMode {
        COLOR_MIP_LEVEL = 0,
        COLOR_EMITTANCE = 1,
        COLOR_OCCLUSION = 2,
        COLOR_TEXELS_LOWER_MIP = 3,
        COLOR_TEXELS_UPPER_MIP = 4,
    };

    ColorMode colorMode = COLOR_MIP_LEVEL;
    bool onlyContributingSamples = false;
    int32_t coneIndexFilter = 0;
    int32_t sampleIndexFilter = 0;
    bool showConeDirections = false;
};

enum class MaterialSamplingRate {
        // Fixed rates, i.e. use the same rasterization resolution for all clipmap LODs.
        FIXED_DEFAULT,
        FIXED_2X,
        FIXED_3X,
        FIXED_4X,
        // Adaptive rates, i.e. coarser clipmap LODs use higher rasterization resolutions.
        // Default:            LOD 0 -> 1x, LOD 1 -> 2x, LOD 2 -> 4x, LOD 3 -> 8x, LOD 4 ->
        // 16x
        ADAPTIVE_DEFAULT,
        // Greater/equal to 2: LOD 0 -> 2x, LOD 1 -> 2x, LOD 2 -> 4x, LOD 3 -> 8x, LOD 4 ->
        // 16x
        ADAPTIVE_GE2,
        // Greater/equal to 4: LOD 0 -> 4x, LOD 1 -> 4x, LOD 2 -> 4x, LOD 3 -> 8x, LOD 4 ->
        // 16x
        ADAPTIVE_GE4
};

struct MaterialInfo {
    // Opacity voxelization thickness in voxels. [0..2]
    // Thickness affects the quality of occlusion produced by an object. Big thickness
    // values should be used for walls, and small ones should be used for objects whose size
    // is comparable to a voxel. When a triangle is voxelized, it covers a set of small
    // cubes. The sides of these cubes are 1/3 voxel, so one voxel contains 27 such cubes.
    // The voxelizationThickness parameter controls how many of these cubes does a triangle
    // cover along its normal. It is measured in voxels for convenience, e.g. when thickness
    // <= 1/3, only one cube is covered; when it's 1.0, three cubes are covered.
    float voxelizationThickness = 1.f;

    // Opacity voxelization anti-aliasing through jitter. Individual samples covered by
    // objects are moved along the voxelization Z axis in order to smoothen the opacity
    // transitions as objects move.
    float opacityNoiseScale = 0.f;  // in voxels, [-1..1]
    float opacityNoiseBias = 0.f;   // in voxels, [-0.5..0.5]

    // Controls whether signed opacity representation of this object should block light in
    // all directions, not just in front-to-back direction
    bool twoSided = false;

    // Set this to true if the geometry is represented in FrontCCW mode
    bool frontCounterClockwise = false;

    // Emittance voxelization anti-aliasing through a triangular filter applied in
    // voxelization Z direction. This filter is almost free in terms of performance, but it
    // makes directly voxelized emittance differ from downsampled emittance. For that
    // reason, this filter should be used mostly on large dynamic objects.
    bool proportionalEmittance = false;

    // Controls whether light emitted by this material is omnidirectional. Set this to true
    // for small objects.
    bool omnidirectionalLight = false;

    // A multiplier for material sampling rate during emittance voxelization, basically for
    // rasterization resolution. It's useful to use higher sampling rates in case shading
    // results cannot be filtered well enough with once-per-voxel shading. That happens for
    // example with materials that have binary masks over textures or colors, because the
    // mask cannot be filtered with standard texture sampling.
    MaterialSamplingRate materialSamplingRate = MaterialSamplingRate::FIXED_DEFAULT;

    // Allows the geometry shader to cull triangles that fit into one page which is not
    // marked for invalidation. Improves performance when application mesh culling is too
    // coarse in incremental voxelization mode.
    bool enableTriangleCulling = true;

    bool requiresNewState(const MaterialInfo& b) const {
        return materialSamplingRate != b.materialSamplingRate ||
               enableTriangleCulling != b.enableTriangleCulling;
    }

    bool requiresParameterUpdate(const MaterialInfo& b) const {
        return voxelizationThickness != b.voxelizationThickness ||
               opacityNoiseScale != b.opacityNoiseScale ||
               opacityNoiseBias != b.opacityNoiseBias || twoSided != b.twoSided ||
               frontCounterClockwise != b.frontCounterClockwise ||
               proportionalEmittance != b.proportionalEmittance ||
               omnidirectionalLight != b.omnidirectionalLight;
    }

    bool operator==(const MaterialInfo& b) const { return !(*this != b); }

    bool operator!=(const MaterialInfo& b) const {
        return requiresNewState(b) || requiresParameterUpdate(b);
    }
};

struct UpdateVoxelizationParameters {
    // Anchor is the point around which the clipmap center is located - it is snapped to a
    // grid. For first-person cameras, the anchor should be placed slightly ahead of the
    // camera, e.g. at (eyePosition + eyeDirection * giRange).
    dm::float3 clipmapAnchor = 0.f;

    // Scene bounding box in world space - used to stop cone tracing when cones exit the
    // scene
    dm::box3 sceneExtents = {dm::float3(FLT_MIN), dm::float3(FLT_MAX)};

    // Size of the finest clipmap level, in world units
    float giRange = 512.f;

    // A set of world-space boxes that contain some geometry that changed since the previous
    // frame
    const dm::box3* invalidatedRegions = nullptr;

    // The number of boxes in 'invalidatedRegions'
    uint32_t invalidatedRegionCount = 0;

    // A set of world-space frusta for lights which have been moved or otherwise changed.
    // All emittance data in pages intersecting the frusta will be cleared.
    const dm::frustum* invalidatedLightFrusta = nullptr;

    // The number of frusta in 'invalidatedLightFrusta'
    uint32_t invalidatedFrustumCount = 0;

    // Parameters that control the cone tracing process used for the indirect irradiance
    // map. The nearClipZ, farClipZ and debugParameters members of CommonTracingParameters
    // are ignored here, all others are effective.
    IndirectIrradianceMapTracingParameters indirectIrradianceMapTracingParameters;
};

struct DebugRenderParameters {
    // Which texture?
    DebugRenderMode debugMode = DebugRenderMode::DISABLED;

    // Camera paramaters
    float4x4 viewMatrix;
    float4x4 projMatrix;

    nvrhi::Viewport viewport;

    // Required
    nvrhi::ITexture* destinationTexture;

    // Optional - use it to correctly overlay the voxels over the scene rendering
    nvrhi::ITexture* destinationDepth;

    nvrhi::BlendState blendState;
    nvrhi::DepthStencilState depthStencilState;

    // Opacity that will be written into the .a channel of destinationTexture for covered
    // pixels
    float targetOpacity = 1.f;

    // Clipmap level to visualize (for opacity and emittance views)
    uint32_t level = 0;

    // Allocation map bit index to visualize (for the allocation map view)
    uint32_t bitToDisplay = 0;

    // Number of voxel faces to look through
    uint32_t voxelsToSkip = 0;

    // Projection parameters
    float nearClipZ = 0.f;
    float farClipZ = 1.f;
};

struct ViewTracerInputBuffers {
    // Handles for G-buffer textures
    nvrhi::ITexture* gbufferDepth = nullptr;   // Depth buffer, required
    nvrhi::ITexture* gbufferNormal = nullptr;  // Normals (.xyz) and roughness (.w), required
    nvrhi::ITexture* gbufferGeoNormal = nullptr;    // Normals without normal maps (.xyz), optional - improves performance
    nvrhi::ITexture* gbufferStencil = nullptr;      // Stencil buffer for alternative tracing settings, optional

    // Parameters of the camera that was used to render the G-buffer, required
    float4x4 viewMatrix;
    float4x4 projMatrix;

    // Viewport within the G-buffer textures, required
    nvrhi::Viewport gbufferViewport = {};

    // Scale and bias for decoding the contents of gbufferNormal and gbufferGeoNormal textures.
    // The effective normal N is computed like this: N = normalize(gbufferNormal.xyz * gbufferNormalScale +
    // gbufferNormalBias)
    float gbufferNormalScale = 1.f;
    float gbufferNormalBias = 0.f;

    // Parameters to determine whether to use alt-settings based on a stencil value.
    // Evaluated as (stencilValue & altSettingsStencilMask) == altSettingsStencilRefValue.
    // Alt-settings are disabled by default.
    int altSettingsStencilMask = 0;
    int altSettingsStencilRefValue = 1;
};

}  // namespace vxgi

#endif /* SRC_VXGISAMPLE_VXGITYPES_H */
