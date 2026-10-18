// Procedural sparse-volume ray tracing for DXR, Vulkan RT and CUDA/OptiX.
// SPDX-License-Identifier: Apache-2.0
#include "GVDBRaycastCommon.slang"
RaytracingAccelerationStructure g_Scene : register(t5);
struct RadiancePayload { float4 color; float depth; };
struct ShadowPayload { float transmission; };
struct VolumeAttributes { float3 normal; float exitT; };

bool reportVolumeHit(float distance,uint kind,VolumeAttributes attributes) {
    __target_switch {
    case cuda:
        // Slang 2026.10.2's typed ReportHit does not lower to CUDA. The explicit
        // integer-attribute overload preserves the same 16-byte attributes.
        return ReportHitOptix(distance,kind,asuint(attributes.normal.x),asuint(attributes.normal.y),asuint(attributes.normal.z),asuint(attributes.exitT));
    default:
        return ReportHit(distance,kind,attributes);
    }
}
float3 surfaceNormal(float3 position,float3 direction) {
    float3 gradient=float3(sampleVolume(position+float3(.5,0,0))-sampleVolume(position-float3(.5,0,0)),
        sampleVolume(position+float3(0,.5,0))-sampleVolume(position-float3(0,.5,0)),
        sampleVolume(position+float3(0,0,.5))-sampleVolume(position-float3(0,0,.5)));
    return dot(gradient,gradient)>1e-12?normalize(-gradient):-normalize(direction);
}
bool volumeInterval(out float enter,out float leave) {
    bool hit=intersectBounds(ObjectRayOrigin(),ObjectRayDirection(),RayTMin(),RayTCurrent(),enter,leave);
    enter=max(enter,RayTMin());leave=min(leave,RayTCurrent());return hit&&enter<leave;
}
[shader("raygeneration")]
void rayGeneration() {
    uint2 pixel=DispatchRaysIndex().xy;
    if(any(pixel>=g_Parameters.image.xy))return;
    RayDesc ray;ray.Origin=g_Parameters.rayOrigin.xyz;ray.Direction=pixelDirection(pixel);
    ray.TMin=g_Parameters.march.y;ray.TMax=g_Parameters.march.z;
    RadiancePayload payload;payload.color=g_Parameters.background;payload.depth=asfloat(0x7f800000u);
    if(any(ray.Direction!=0))TraceRay(g_Scene,RAY_FLAG_NONE,255,0,2,0,ray,payload);
    uint index=pixel.y*g_Parameters.image.x+pixel.x;
    g_Output[index]=payload.color;g_Depth[index]=payload.depth;
}
[shader("miss")]
void radianceMiss(inout RadiancePayload payload) {
    payload.color=g_Parameters.background;payload.depth=asfloat(0x7f800000u);
}
[shader("miss")]
void shadowMiss(inout ShadowPayload payload) {}
[shader("intersection")]
void volumeIntersection() {
    float enter,leave;
    if(volumeInterval(enter,leave)) {
        VolumeAttributes attributes;attributes.normal=float3(0);attributes.exitT=leave;
        reportVolumeHit(enter,0,attributes);
    }
}
[shader("intersection")]
void surfaceIntersection() {
    float enter,leave;
    if(!volumeInterval(enter,leave))return;
    float3 origin=ObjectRayOrigin(),direction=ObjectRayDirection();
    // Ray t remains in world units under instance transforms; march in index units.
    float stepSize=g_Parameters.march.x/length(direction);
    float previousT=enter,previous=sampleVolume(origin+direction*enter)-g_Parameters.valueRange.z;
    for(uint step=0;step<g_Parameters.image.w;++step) {
        float t=min(enter+float(step+1)*stepSize,leave);
        float value=sampleVolume(origin+direction*t)-g_Parameters.valueRange.z;
        if((previous<=0&&value>=0)||(previous>=0&&value<=0)) {
            float low=previousT,high=t;
            for(uint refine=0;refine<8;++refine) {
                float mid=(low+high)*.5;
                float sample=sampleVolume(origin+direction*mid)-g_Parameters.valueRange.z;
                if((previous<=0&&sample<=0)||(previous>=0&&sample>=0))low=mid;else high=mid;
            }
            float hit=(low+high)*.5;
            VolumeAttributes attributes;attributes.normal=surfaceNormal(origin+direction*hit,direction);attributes.exitT=leave;
            if(reportVolumeHit(hit,1,attributes))return;
        }
        if(t>=leave)return;
        previousT=t;previous=value;
    }
}
float volumeTransmittance(float enter,float leave,out float3 integratedColor,out float firstDepth) {
    float3 origin=mul(WorldToObject3x4(),float4(WorldRayOrigin(),1));
    float3 direction=mul((float3x3)WorldToObject3x4(),WorldRayDirection());
    float scale=length(direction),stepSize=g_Parameters.march.x/scale;
    float transmission=1;integratedColor=float3(0);firstDepth=asfloat(0x7f800000u);
    float t=enter;
    for(uint step=0;step<g_Parameters.image.w&&t<leave;++step) {
        float distance=min(stepSize,leave-t);
        float4 transfer=transferValue(sampleVolume(origin+direction*(t+distance*.5)));
        float opacity=1-exp(-max(0.0,transfer.w)*g_Parameters.march.w*distance*scale);
        if(opacity>0&&isinf(firstDepth))firstDepth=t;
        integratedColor+=transmission*opacity*transfer.xyz;transmission*=1-opacity;t+=distance;
        if(1-transmission>=g_Parameters.valueRange.w)break;
    }
    return transmission;
}
[shader("closesthit")]
void volumeClosestHit(inout RadiancePayload payload,VolumeAttributes attributes) {
    float3 color;float depth;
    float transmission=volumeTransmittance(RayTCurrent(),attributes.exitT,color,depth);
    payload.color=float4(color+transmission*g_Parameters.background.xyz,1-transmission+transmission*g_Parameters.background.w);
    payload.depth=depth;
}
[shader("closesthit")]
void surfaceClosestHit(inout RadiancePayload payload,VolumeAttributes attributes) {
    float3 worldPosition=WorldRayOrigin()+WorldRayDirection()*RayTCurrent();
    float3 normal=normalize(mul(transpose((float3x3)WorldToObject3x4()),attributes.normal));
    float3 towardLight=g_Parameters.light.xyz-worldPosition;
    float lightDistance=length(towardLight);
    float diffuse=lightDistance>1e-6?max(0.0,dot(normal,towardLight/lightDistance)):0;
    ShadowPayload shadow;shadow.transmission=1;
    if(diffuse>0&&lightDistance>2e-4) {
        RayDesc ray;ray.Origin=worldPosition;ray.Direction=towardLight/lightDistance;ray.TMin=1e-4;ray.TMax=lightDistance-1e-4;
        TraceRay(g_Scene,RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,255,1,2,1,ray,shadow);
    }
    float illumination=g_Parameters.light.w+(1-g_Parameters.light.w)*diffuse*shadow.transmission;
    payload.color=float4(transferValue(g_Parameters.valueRange.z).xyz*illumination,1);payload.depth=RayTCurrent();
}
[shader("anyhit")]
void volumeShadowAnyHit(inout ShadowPayload payload,VolumeAttributes attributes) {
    float3 ignored;float depth;
    payload.transmission*=volumeTransmittance(RayTCurrent(),attributes.exitT,ignored,depth);
    if(payload.transmission<=1-g_Parameters.valueRange.w)AcceptHitAndEndSearch();
    else IgnoreHit();
}
[shader("anyhit")]
void surfaceShadowAnyHit(inout ShadowPayload payload,VolumeAttributes attributes) {
    payload.transmission=0;AcceptHitAndEndSearch();
}
