#ifndef BEACH_AERIAL_PERSPECTIVE_HLSLI
#define BEACH_AERIAL_PERSPECTIVE_HLSLI
// A view-frustum volume stored as a 2D atlas for the existing D3D11 texture
// path. Distance zero is an explicit slice (L=0, T=1), not a hand-tuned fade.
static const uint BeachAerialWidth = 64;
static const uint BeachAerialHeight = 64;
static const uint BeachAerialSlices = 32;
// Put angular samples around the geometric horizon, where optical depth
// changes rapidly. This is a LUT coordinate transform, not radiance blending.
float BeachAerialScreenY(float v, float horizon)
{
    if (horizon <= .001 || horizon >= .999) return v;
    float t = 2 * v - 1;
    return t < 0 ? horizon * (1 - t * t) : horizon + (1 - horizon) * t * t;
}
float BeachAerialVolumeY(float v, float horizon)
{
    if (horizon <= .001 || horizon >= .999) return v;
    return v < horizon ? .5 - .5 * sqrt(saturate(1 - v / horizon)) :
        .5 + .5 * sqrt(saturate((v - horizon) / (1 - horizon)));
}
float BeachAerialSliceDistance(float slice, float farDistance)
{
    return exp2(slice / (BeachAerialSlices - 1) * log2(1.0 + farDistance)) - 1.0;
}
float BeachAerialDistanceSlice(float distance, float farDistance)
{
    return saturate(log2(1.0 + max(distance, 0.0)) / log2(1.0 + farDistance)) *
        (BeachAerialSlices - 1);
}
float2 BeachAerialAtlasUv(float2 screenUv, float slice, float2 horizon)
{
    screenUv.y = BeachAerialVolumeY(screenUv.y, horizon.x + (screenUv.x - .5) * horizon.y);
    float2 pixel = clamp(screenUv * float2(BeachAerialWidth, BeachAerialHeight),
        0.5, float2(BeachAerialWidth, BeachAerialHeight) - 0.5);
    return (pixel + float2(slice * BeachAerialWidth, 0)) /
        float2(BeachAerialWidth * BeachAerialSlices, BeachAerialHeight);
}
#endif
