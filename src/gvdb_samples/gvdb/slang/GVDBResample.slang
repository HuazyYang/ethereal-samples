// Legacy GVDB region placement, retained for sparse resample activation.
// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct OperatorParameters {VolumeInfo volume;float4 value;uint4 sourceResolution;float4 transformRows[3];float4 remap;};
ConstantBuffer<OperatorParameters> g_Parameters:register(b0);
StructuredBuffer<float> g_Source:register(t0);
RWStructuredBuffer<float> g_Destination:register(u0);
struct RegionStatistics {float sum;uint count;};
float3 transformPoint(float3 value) {
    float4 point=float4(value,1);
    return float3(dot(g_Parameters.transformRows[0],point),dot(g_Parameters.transformRows[1],point),dot(g_Parameters.transformRows[2],point));
}
RegionStatistics regionStatistics(uint3 id) {
    uint3 destination=g_Parameters.volume.atlasResolutionAndApron.xyz;
    // The original activation samples use res+1 and inclusive, truncated
    // endpoints. A reflected endpoint is clamped upward, not reordered.
    float3 lowerWorld=float3(id)*g_Parameters.value.xyz/float3(destination+1u);
    float3 upperWorld=float3(id+1u)*g_Parameters.value.xyz/float3(destination+1u)-1;
    int3 maximum=int3(g_Parameters.sourceResolution.xyz)-1;
    int3 lower=clamp(int3(transformPoint(lowerWorld)),int3(0),maximum);
    int3 upper=clamp(int3(transformPoint(upperWorld)),lower,maximum);
    float sum=0;uint count=0;
    [loop] for(int z=lower.z;z<=upper.z;++z)
        [loop] for(int y=lower.y;y<=upper.y;++y)
            [loop] for(int x=lower.x;x<=upper.x;++x) {
                float value=g_Source[flatten3(uint3(x,y,z),g_Parameters.sourceResolution.xyz)];
                sum+=g_Parameters.remap.z+(value-g_Parameters.remap.x)*(g_Parameters.remap.w-g_Parameters.remap.z)/(g_Parameters.remap.y-g_Parameters.remap.x);
                ++count;
            }
    RegionStatistics result;result.sum=sum;result.count=count;return result;
}
// Retain the direct GPU average entry for callers that consume a GPU buffer.
[shader("compute")][numthreads(8,8,8)]
void downsampleRegion(uint3 id:SV_DispatchThreadID) {
    uint3 destination=g_Parameters.volume.atlasResolutionAndApron.xyz;
    if(any(id>=destination))return;
    RegionStatistics statistics=regionStatistics(id);
    g_Destination[flatten3(id,destination)]=statistics.sum/float(statistics.count);
}
