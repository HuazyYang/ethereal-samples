// Portable mixed mesh/sparse-volume ray tracing. SPDX-License-Identifier: Apache-2.0
// Layouts mirror SlangSceneRTData.h. Resource arrays contain eight valid descriptors.
#include "GVDBTypes.slang"
struct SceneCamera {
    VolumeInfo volume; uint4 image;
    float4 rayOrigin, direction00, directionDx, directionDy;
    float4 march, valueRange, background, light;
};
struct SceneParameters { SceneCamera camera; uint4 state, random; float4 pointLight; };
struct SceneVolume {
    VolumeInfo info;
    float4 steps, extinct, cutoff, threshold;
    uint4 options; // resource slot, color enabled, inclusive transfer endpoint, volume mode
};
struct SceneMaterial {
    float4 ambient, diffuse, specular, environment;
    float4 reflection, refraction, optical, bias;
};
ConstantBuffer<SceneParameters> g_SceneParameters : register(b0);
RaytracingAccelerationStructure g_Scene : register(t0);
StructuredBuffer<uint4> g_Instances : register(t1);
StructuredBuffer<uint4> g_Meshes : register(t2);
StructuredBuffer<float4> g_Positions : register(t3);
StructuredBuffer<float4> g_Normals : register(t4);
StructuredBuffer<uint> g_Indices : register(t5);
StructuredBuffer<SceneVolume> g_Volumes : register(t6);
StructuredBuffer<SceneMaterial> g_Materials : register(t7);
Texture2D<float4> g_Environment : register(t8);
SamplerState g_EnvironmentSampler : register(s0);
StructuredBuffer<float> g_Scalars[8] : register(t16);
ByteAddressBuffer g_Nodes[8] : register(t24);
ByteAddressBuffer g_Children[8] : register(t32);
StructuredBuffer<LevelInfo> g_Levels[8] : register(t40);
ByteAddressBuffer g_Colors[8] : register(t48);
StructuredBuffer<float4> g_Transfers[8] : register(t56);
RWStructuredBuffer<float4> g_Output : register(u0);
RWStructuredBuffer<float> g_Depth : register(u1);
RWStructuredBuffer<float4> g_Accumulation : register(u2);
#define g_Parameters g_SceneParameters.camera
#include "GVDBRayClip.slang"
#undef g_Parameters

struct ScenePayload { float4 result; float distance; float alpha; uint depth; uint seed; };
struct SceneAttributes { float3 normal; float backT; uint color; };
static const uint RADIANCE = 0, SHADOW = 1, REFRACTION = 2;
static const uint MESH_MASK = 1, SURFACE_MASK = 2, LEVEL_MASK = 4, DEEP_MASK = 8;
static const float FAR_DISTANCE = 1e16;
float3 safeNormal(float3 value, float3 fallback) {
    float square = dot(value,value);
    return square > 1e-20 && isfinite(square) ? value*rsqrt(square) : fallback;
}
uint packColor(float4 value) {
    uint4 bytes = uint4(saturate(value)*255+.5);
    return bytes.x | (bytes.y<<8) | (bytes.z<<16) | (bytes.w<<24);
}
float4 unpackColor(uint value) {
    return float4(value&255,(value>>8)&255,(value>>16)&255,(value>>24)&255)/255;
}
float randomValue(inout uint seed) {
    seed = 1664525u*seed+1013904223u;
    return float(seed&0x00ffffffu)/16777216.0;
}
float3 jitter(inout uint seed) {
    float x=randomValue(seed),y=randomValue(seed),z=randomValue(seed);
    return float3(x,y,z)-.5;
}
uint initialSeed(uint pixel) {
    // Stable independent per-pixel seeds replace legacy srand/modulo seed sharing.
    uint seed = pixel ^ (g_SceneParameters.random.x*0x9e3779b9u) ^
        (g_SceneParameters.random.y+g_SceneParameters.state.x*17u+4732u);
    seed ^= seed>>16; seed *= 0x7feb352du; seed ^= seed>>15; seed *= 0x846ca68bu;
    return seed^(seed>>16);
}
float3 environment(float3 direction) {
    float v=1-direction.y;
    if(v>1)return float3(.1);
    if(g_SceneParameters.state.w==0)return float3(1);
    float u=atan2(direction.x,direction.z)/3.141592653589793;
    return g_Environment.SampleLevel(g_EnvironmentSampler,float2(u,saturate(v)),0).xyz;
}
uint volumeSlot(SceneVolume volume) { return NonUniformResourceIndex(volume.options.x); }
bool volumeLeaf(SceneVolume volume,float3 position,out VDBNode leaf) {
    uint ignored;uint slot=volumeSlot(volume);
    return findLeaf(g_Nodes[slot],g_Children[slot],g_Levels[slot],volume.info,position,leaf,ignored);
}
bool voxelAddress(SceneVolume volume,int3 cell,out uint address) {
    VDBNode leaf;address=0;
    if(!volumeLeaf(volume,float3(cell)+.5,leaf))return false;
    address=flatten3(uint3(leaf.atlasPosition+cell-leaf.position),volume.info.atlasResolutionAndApron.xyz);
    return true;
}
float scalarValue(SceneVolume volume,float3 position,out bool supported) {
    supported=false;
    if(!all(isfinite(position))||any(abs(position)>8388607))return volume.threshold.w;
    float3 centered=position-.5;int3 lower=int3(floor(centered));float3 fraction=centered-float3(lower);
    uint slot=volumeSlot(volume);VDBNode leaf;
    if(volumeLeaf(volume,float3(lower)+.5,leaf)) {
        int3 local=lower-leaf.position;
        if(all(local>=0)&&all(local+1<g_Levels[slot][0].nodeExtent.xyz)) {
            supported=true;float result=0;
            for(uint corner=0;corner<8;++corner) {
                int3 offset=int3(corner&1,(corner>>1)&1,(corner>>2)&1);
                float3 w=lerp(1-fraction,fraction,float3(offset));
                result+=w.x*w.y*w.z*g_Scalars[slot][flatten3(uint3(leaf.atlasPosition+local+offset),volume.info.atlasResolutionAndApron.xyz)];
            }
            return result;
        }
    }
    float result=0;
    for(uint corner=0;corner<8;++corner) {
        int3 offset=int3(corner&1,(corner>>1)&1,(corner>>2)&1);
        float3 w=lerp(1-fraction,fraction,float3(offset));float weight=w.x*w.y*w.z;
        if(weight<=0)continue;
        uint address;bool present=voxelAddress(volume,lower+offset,address);
        supported=supported||present;
        result+=weight*(present?g_Scalars[slot][address]:volume.threshold.w);
    }
    return result;
}
float scalarValue(SceneVolume volume,float3 position) {bool supported;return scalarValue(volume,position,supported);}
float3 volumeColor(SceneVolume volume,float3 position) {
    if(volume.options.y==0)return float3(1);
    uint address;
    if(!voxelAddress(volume,int3(floor(position)),address))return float3(1);
    return unpackColor(g_Colors[volumeSlot(volume)].Load(address*4)).xyz;
}
float3 volumeNormal(SceneVolume volume,float3 position,float3 fallback,bool levelSet) {
    float3 gradient=float3(scalarValue(volume,position+float3(.5,0,0))-scalarValue(volume,position-float3(.5,0,0)),
        scalarValue(volume,position+float3(0,.5,0))-scalarValue(volume,position-float3(0,.5,0)),
        scalarValue(volume,position+float3(0,0,.5))-scalarValue(volume,position-float3(0,0,.5)));
    return safeNormal(levelSet?gradient:-gradient,fallback);
}
bool volumeInterval(SceneVolume volume,float3 origin,float3 direction,float maximum,out float enter,out float leave) {
    enter=RayTMin();leave=maximum;
    for(uint axis=0;axis<3;++axis) {
        float lo=volume.info.boundsMin[axis],hi=volume.info.boundsMax[axis];
        if(direction[axis]==0) { if(origin[axis]<lo||origin[axis]>=hi)return false; }
        else {float a=(lo-origin[axis])/direction[axis],b=(hi-origin[axis])/direction[axis];
            enter=max(enter,min(a,b));leave=min(leave,max(a,b));}
    }
    return enter<leave;
}
// Return the exit of the leaf-grid cell at position. Empty cells can be skipped
// without assuming the contents of their (possibly stale) scalar aprons.
float gridCellExit(SceneVolume volume,float3 origin,float3 direction,float t) {
    uint slot=volumeSlot(volume);LevelInfo top=g_Levels[slot][volume.info.hierarchy.x];
    VDBNode root=loadNode(g_Nodes[slot],top.nodes.x+volume.info.hierarchy.y*top.nodes.y);
    float3 extent=float3(g_Levels[slot][0].nodeExtent.xyz),position=origin+direction*t;
    float3 cell=floor((position-float3(root.position))/extent)*extent+float3(root.position);
    float end=FAR_DISTANCE;
    for(uint axis=0;axis<3;++axis)if(direction[axis]!=0) {
        float boundary=direction[axis]>0?cell[axis]+extent[axis]:cell[axis];
        float next=(boundary-origin[axis])/direction[axis];
        if(next>=t)end=min(end,next);
    }
    return end;
}
float leafWavefront(SceneVolume volume,VDBNode leaf,float3 origin,float3 direction,float scale,bool emptyMode) {
    float enter=-FAR_DISTANCE;
    float3 lo=float3(leaf.position),hi=lo+float3(g_Levels[volumeSlot(volume)][0].nodeExtent.xyz);
    for(uint axis=0;axis<3;++axis)if(direction[axis]!=0) {
        float a=(lo[axis]-origin[axis])/direction[axis],b=(hi[axis]-origin[axis])/direction[axis];
        enter=max(enter,min(a,b));
    }
    float start=enter*scale+volume.steps.w;
    return (emptyMode?start:ceil(start/volume.steps.x)*volume.steps.x)/scale;
}
bool reportSceneHit(float distance,uint kind,SceneAttributes attributes) {
    __target_switch {
    case cuda:
        return ReportHitOptix(distance,kind,asuint(attributes.normal.x),asuint(attributes.normal.y),
            asuint(attributes.normal.z),asuint(attributes.backT),attributes.color);
    default: return ReportHit(distance,kind,attributes);
    }
}
SceneVolume currentVolume() { return g_Volumes[g_Instances[InstanceID()].y]; }
void intersectSurface(bool levelSet,bool emptyMode,uint hitKind) {
    SceneVolume volume=currentVolume();float3 origin=ObjectRayOrigin(),direction=ObjectRayDirection();
    float scale=length(direction),enter,leave;if(!(scale>0)||!volumeInterval(volume,origin,direction,RayTCurrent(),enter,leave))return;
    float step=volume.steps.x/scale;
    float t=emptyMode?enter+volume.steps.w/scale:ceil((enter*scale+volume.steps.w)/volume.steps.x)*step;
    for(uint iteration=0;iteration<g_SceneParameters.camera.image.w&&t<leave;++iteration) {
        float3 position=origin+direction*t;VDBNode leaf;
        if(!volumeLeaf(volume,position,leaf)) {
            float next=gridCellExit(volume,origin,direction,t);
            if(next<t||next>=leave)return;
            float advanced=emptyMode?next+volume.steps.w/scale:
                max(t+step,ceil((next*scale+volume.steps.w)/volume.steps.x)*step);
            t=advanced>t?advanced:asfloat(asuint(t)+1u);continue;
        }
        float aligned=leafWavefront(volume,leaf,origin,direction,scale,emptyMode);
        if(aligned>t){t=aligned;continue;}
        bool supported;float scalar=scalarValue(volume,position,supported);
        if(emptyMode||(supported&&(levelSet?scalar<volume.threshold.x:scalar>=volume.threshold.x))) {
            SceneAttributes attributes;
            attributes.normal=emptyMode?float3(0):volumeNormal(volume,position,-direction/scale,levelSet);
            attributes.backT=leave;attributes.color=packColor(float4(emptyMode?float3(1):volumeColor(volume,position),1));
            if(reportSceneHit(t,hitKind,attributes))return;
        }
        float next=t+step;if(!(next>t))return;t=next;
    }
}
[shader("intersection")] void sceneSurfaceIntersection(){intersectSurface(false,false,1);}
[shader("intersection")] void sceneLevelSetIntersection(){intersectSurface(true,false,3);}
[shader("intersection")] void sceneLevelSetRefractionIntersection(){intersectSurface(false,false,3);}
[shader("intersection")] void sceneEmptyIntersection(){intersectSurface(false,true,4);}
// Integrate in the hit shader, where the payload identifies primary versus
// secondary rays. RayTCurrent may already be the nearest triangle distance;
// it cannot be used as the deep integration limit.
uint integrateDeep(float maximum) {
    SceneVolume volume=currentVolume();
    // OptiX object-ray getters are restricted to intersection/any-hit. The
    // world-to-object transform is valid in closest-hit on every backend.
    float3 origin=mul(WorldToObject3x4(),float4(WorldRayOrigin(),1));
    float3 direction=mul((float3x3)WorldToObject3x4(),WorldRayDirection());
    float scale=length(direction),enter,leave;
    if(!(scale>0)||!volumeInterval(volume,origin,direction,maximum,enter,leave))return 0;
    float step=volume.steps.x/scale,t=ceil((enter*scale+volume.steps.w)/volume.steps.x)*step;
    float4 integrated=float4(0,0,0,1);
    for(uint iteration=0;iteration<g_SceneParameters.camera.image.w&&t<leave&&integrated.w>volume.cutoff.y;++iteration) {
        float3 position=origin+direction*t;VDBNode leaf;
        if(!volumeLeaf(volume,position,leaf)) {
            float next=gridCellExit(volume,origin,direction,t);
            if(next<t||next>=leave)break;
            t=max(t+step,ceil((next*scale+volume.steps.w)/volume.steps.x)*step);continue;
        }
        float aligned=leafWavefront(volume,leaf,origin,direction,scale,false);
        if(aligned>t){t=aligned;continue;}
        float value=scalarValue(volume,position);
        if(value>=volume.cutoff.x) {
            uint index=uint(saturate((value-volume.threshold.x)/(volume.threshold.z-volume.threshold.y))*float(volume.options.z));
            float4 transfer=g_Transfers[volumeSlot(volume)][index];
            float transmission=exp(volume.extinct.x*transfer.w*volume.steps.x);
            integrated.xyz+=transfer.xyz*integrated.w*(1-transmission)*volume.extinct.y*volumeColor(volume,position);
            integrated.w*=transmission;
        }
        float next=t+step;if(!(next>t))break;t=next;
    }
    return packColor(float4(min(integrated.xyz,float3(1)),1-integrated.w));
}
[shader("intersection")]
void sceneDeepIntersection() {
    SceneVolume volume=currentVolume();float3 origin=ObjectRayOrigin(),direction=ObjectRayDirection();
    if(volume.cutoff.y>=1)return;
    float scale=length(direction),enter,leave;
    if(!(scale>0)||!volumeInterval(volume,origin,direction,RayTCurrent(),enter,leave))return;
    float step=volume.steps.x/scale,t=ceil((enter*scale+volume.steps.w)/volume.steps.x)*step;
    for(uint iteration=0;iteration<g_SceneParameters.camera.image.w&&t<leave;++iteration) {
        VDBNode leaf;
        if(!volumeLeaf(volume,origin+direction*t,leaf)) {
            float next=gridCellExit(volume,origin,direction,t);
            if(next<t||next>=leave)return;
            float advanced=max(t+step,ceil((next*scale+volume.steps.w)/volume.steps.x)*step);
            if(!(advanced>t))return;
            t=advanced;continue;
        }
        float aligned=leafWavefront(volume,leaf,origin,direction,scale,false);
        if(aligned>t){t=aligned;continue;}
        SceneAttributes attributes;attributes.normal=float3(0,1,0);attributes.backT=leave;attributes.color=0;
        reportSceneHit(t,2,attributes);return;
    }
}

ScenePayload traceScene(float3 origin,float3 direction,uint depth,uint purpose,uint mask,inout uint seed) {
    ScenePayload payload;payload.result=float4(0);payload.distance=0;payload.alpha=1;payload.depth=depth;payload.seed=seed;
    RayDesc ray;ray.Origin=origin;ray.Direction=direction;ray.TMin=0;ray.TMax=FAR_DISTANCE;
    // Deep continuation keeps the primary camera interval. Reflected,
    // refracted and shadow rays retain their independent unlimited range.
    if(depth==0&&purpose==RADIANCE) {
        float2 clip=rayClipInterval(direction);ray.TMin=clip.x;ray.TMax=clip.y;
    }
    if(purpose==REFRACTION&&depth==2)mask&=~LEVEL_MASK;
    uint flags=purpose==SHADOW?(RAY_FLAG_FORCE_NON_OPAQUE|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER):RAY_FLAG_NONE;
    TraceRay(g_Scene,flags,mask,purpose,3,purpose,ray,payload);
    seed=payload.seed;return payload;
}
bool refractDirection(float3 incoming,float3 normal,float ior,out float3 refracted) {
    float cosine=dot(incoming,normal);float eta;
    if(cosine>0){eta=ior;normal=-normal;cosine=-cosine;}else eta=1/ior;
    float k=1-eta*eta*(1-cosine*cosine);refracted=float3(0);
    if(k<0)return false;
    refracted=safeNormal(eta*incoming-(eta*cosine+sqrt(k))*normal,float3(0));return any(refracted!=0);
}
void shadeSurface(inout ScenePayload payload,SceneMaterial material,float3 normal,float3 position,float3 tint) {
    float3 incoming=WorldRayDirection(),origin=WorldRayOrigin();
    float3 noise=jitter(payload.seed);
    float3 towardLight=safeNormal(g_SceneParameters.pointLight.xyz-position,normal);
    float3 light=safeNormal(towardLight+noise*material.ambient.w,towardLight);
    float3 diffuse=material.diffuse.xyz*environment(light)*max(0.0,dot(normal,light));
    float3 halfDirection=safeNormal(-incoming+towardLight,normal);
    float3 specular=material.specular.xyz*pow(max(0.0,dot(normal,halfDirection)),material.diffuse.w);
    float3 reflection=0,refraction=0;float shadow=1;
    if(payload.depth<1&&material.reflection.w>0) {
        float3 reflected=safeNormal(reflect(incoming,normal),-incoming);
        reflected=safeNormal(reflected+noise*material.reflection.w,reflected);
        ScenePayload child=traceScene(position+reflected*material.optical.x,reflected,payload.depth+1,RADIANCE,15,payload.seed);
        reflection=child.result.xyz*material.reflection.xyz;
    }
    if(payload.depth<2&&material.refraction.w>0) {
        float3 refracted;
        if(refractDirection(incoming,normal,material.optical.y,refracted)) {
            refracted=safeNormal(refracted+noise*material.refraction.w,refracted);
            ScenePayload child=traceScene(position+refracted*material.bias.x,refracted,payload.depth+1,REFRACTION,15,payload.seed);
            refraction=lerp(child.result.xyz*material.optical.z,material.refraction.xyz,min(1.0,child.distance/material.optical.w));
        }
    }
    if(payload.depth<1)for(uint sample=0;sample<2;++sample) {
        float3 shadowDirection=safeNormal(towardLight+jitter(payload.seed)*material.ambient.w,towardLight);
        ScenePayload child=traceScene(position+shadowDirection*material.environment.w,shadowDirection,payload.depth+1,SHADOW,15,payload.seed);
        shadow*=child.alpha;
    }
    if(material.environment.x==1&&material.environment.y!=0) {
        float parity=fmod(floor(position.x/material.environment.y)+floor(position.z/material.environment.y),2.0);
        diffuse*=parity==0?1:material.environment.z;
    }
    payload.result=float4((diffuse*tint+specular+material.ambient.xyz)*shadow+
        (reflection+refraction)*(shadow*.3+.7),1);
    payload.distance=length(position-origin);payload.alpha=0;
}
void meshShade(inout ScenePayload payload,BuiltInTriangleIntersectionAttributes attributes) {
    uint4 instance=g_Instances[InstanceID()],mesh=g_Meshes[instance.y];
    uint first=mesh.y+PrimitiveIndex()*3;
    uint3 indices=uint3(g_Indices[first],g_Indices[first+1],g_Indices[first+2])+mesh.x;
    float3 a=g_Positions[indices.x].xyz,b=g_Positions[indices.y].xyz,c=g_Positions[indices.z].xyz;
    float3 normal=g_Normals[indices.x].xyz*(1-attributes.barycentrics.x-attributes.barycentrics.y)+
        g_Normals[indices.y].xyz*attributes.barycentrics.x+g_Normals[indices.z].xyz*attributes.barycentrics.y;
    normal=safeNormal(normal,safeNormal(cross(b-a,c-a),float3(0,1,0)));
    normal=safeNormal(mul(transpose((float3x3)WorldToObject3x4()),normal),-WorldRayDirection());
    if(dot(normal,WorldRayDirection())>0)normal=-normal;
    float3 position=WorldRayOrigin()+RayTCurrent()*WorldRayDirection();
    float scale=max(1.0,max(abs(position.x),max(abs(position.y),abs(position.z))));
    shadeSurface(payload,g_Materials[instance.z],normal,position+normal*(1e-4*scale),float3(1));
}
void surfaceShade(inout ScenePayload payload,SceneAttributes attributes) {
    uint4 instance=g_Instances[InstanceID()];
    float3 normal=safeNormal(mul(transpose((float3x3)WorldToObject3x4()),attributes.normal),-WorldRayDirection());
    float3 position=WorldRayOrigin()+RayTCurrent()*WorldRayDirection();
    if(HitKind()==1)position+=mul((float3x3)ObjectToWorld3x4(),attributes.normal*2);
    shadeSurface(payload,g_Materials[instance.z],normal,position,unpackColor(attributes.color).xyz);
}
void deepShade(inout ScenePayload payload,SceneAttributes attributes) {
    float maximum=payload.depth==0?rayClipInterval(WorldRayDirection()).y:FAR_DISTANCE;
    float4 deep=unpackColor(integrateDeep(maximum));
    ScenePayload background=traceScene(WorldRayOrigin(),WorldRayDirection(),payload.depth,RADIANCE,15&~DEEP_MASK,payload.seed);
    payload.result=float4(lerp(background.result.xyz,deep.xyz,deep.w),1);
    payload.distance=RayTCurrent()*length(WorldRayDirection());payload.alpha=deep.w;
}
[shader("closesthit")] void sceneMeshRadiance(inout ScenePayload payload,BuiltInTriangleIntersectionAttributes attributes){meshShade(payload,attributes);}
[shader("closesthit")] void sceneMeshRefraction(inout ScenePayload payload,BuiltInTriangleIntersectionAttributes attributes){meshShade(payload,attributes);}
[shader("closesthit")] void sceneSurfaceRadiance(inout ScenePayload payload,SceneAttributes attributes){surfaceShade(payload,attributes);}
[shader("closesthit")] void sceneSurfaceRefraction(inout ScenePayload payload,SceneAttributes attributes){surfaceShade(payload,attributes);}
[shader("closesthit")] void sceneDeepRadiance(inout ScenePayload payload,SceneAttributes attributes){deepShade(payload,attributes);}
[shader("closesthit")] void sceneDeepRefraction(inout ScenePayload payload,SceneAttributes attributes){deepShade(payload,attributes);}
[shader("anyhit")] void sceneMeshShadow(inout ScenePayload payload,BuiltInTriangleIntersectionAttributes attributes){payload.alpha=0;AcceptHitAndEndSearch();}
[shader("anyhit")] void sceneSurfaceShadow(inout ScenePayload payload,SceneAttributes attributes){payload.alpha=0;AcceptHitAndEndSearch();}
[shader("anyhit")] void sceneDeepShadow(inout ScenePayload payload,SceneAttributes attributes){payload.alpha=1-unpackColor(integrateDeep(FAR_DISTANCE)).w;AcceptHitAndEndSearch();}
void missRadiance(inout ScenePayload payload) {
    payload.result=float4(environment(WorldRayDirection())*g_SceneParameters.camera.background.xyz,1);
    payload.distance=1e10;
}
[shader("miss")] void sceneMissRadiance(inout ScenePayload payload){missRadiance(payload);}
[shader("miss")] void sceneMissRefraction(inout ScenePayload payload){missRadiance(payload);}
[shader("miss")] void sceneMissShadow(inout ScenePayload payload){}
[shader("raygeneration")]
void sceneRaygen() {
    uint2 pixel=DispatchRaysIndex().xy;SceneCamera camera=g_SceneParameters.camera;
    if(any(pixel>=camera.image.xy))return;
    uint index=pixel.y*camera.image.x+pixel.x,seed=initialSeed(index),copySeed=seed;
    // Primary jitter does not consume the path's RNG, as in the reference.
    float3 noise=jitter(copySeed)*length(camera.directionDx.xyz);
    float3 direction=safeNormal(camera.direction00.xyz+(float(pixel.x)+.5)*camera.directionDx.xyz+
        (float(pixel.y)+.5)*camera.directionDy.xyz+noise,float3(0));
    float2 clip=rayClipInterval(direction);
    ScenePayload payload;payload.result=camera.background;payload.distance=asfloat(0x7f800000u);
    payload.alpha=1;payload.depth=0;payload.seed=seed;
    if(any(direction!=0)&&clip.x<clip.y) {
        RayDesc ray;ray.Origin=camera.rayOrigin.xyz;ray.Direction=direction;ray.TMin=clip.x;ray.TMax=clip.y;
        TraceRay(g_Scene,RAY_FLAG_NONE,15,RADIANCE,3,RADIANCE,ray,payload);
    }
    float4 value=payload.result;
    uint sample=g_SceneParameters.state.x;
    if(sample>0)value=(g_Accumulation[index]*float(sample)+value)/float(sample+1);
    g_Accumulation[index]=value;g_Output[index]=value;g_Depth[index]=payload.distance;
}
