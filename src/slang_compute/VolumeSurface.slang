// Sparse positive-outside level sets, with legacy point-filtered RGBA8 color.
// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct SurfaceParameters {
    VolumeInfo volume;
    uint4 image;
    float4 rayOrigin;
    float4 direction00;
    float4 directionDx;
    float4 directionDy;
    float4 march;
    float4 valueRange;
    float4 background;
    float4 light;
    float4 options; // scalar background, has packed color, active publication (raw only), reserved
};
ConstantBuffer<SurfaceParameters> g_Parameters : register(b0);
#if defined(GVDB_SURFACE_RAW)
ByteAddressBuffer g_Atlas : register(t0);
#else
StructuredBuffer<float> g_Atlas : register(t0);
#endif
ByteAddressBuffer g_Nodes : register(t1);
ByteAddressBuffer g_Children : register(t2);
StructuredBuffer<LevelInfo> g_Levels : register(t3);
StructuredBuffer<float4> g_Transfer : register(t4);
ByteAddressBuffer g_Color : register(t5);
RWStructuredBuffer<float4> g_Output : register(u0);
RWStructuredBuffer<float> g_Depth : register(u1);
#include "GVDBRayClip.slang"

float loadAtlasScalar(uint address) {
#if defined(GVDB_SURFACE_RAW)
    return asfloat(g_Atlas.Load(address*4u));
#else
    return g_Atlas[address];
#endif
}
bool voxelAddress(int3 cell,out uint address) {
    address=0;
    VDBNode leaf;uint index;
    if(!findLeaf(g_Nodes,g_Children,g_Levels,g_Parameters.volume,float3(cell)+.5,leaf,index))return false;
    uint3 atlas=leaf.atlasPosition+uint3(cell-leaf.position);
    address=flatten3(atlas,g_Parameters.volume.atlasResolutionAndApron.xyz);
    return true;
}
float scalarValue(float3 position,out bool supported) {
    supported=false;
    if(!all(isfinite(position)))return g_Parameters.options.x;
    float3 centered=position-.5;
    int3 lower=int3(floor(centered));float3 fraction=centered-float3(lower);
    // A footprint wholly inside one leaf cell needs one hierarchy lookup.
    // The root defines the cell grid, including nonzero/negative origins.
    LevelInfo rootInfo=g_Levels[g_Parameters.volume.hierarchy.x];
    VDBNode root=loadNode(g_Nodes,rootInfo.nodes.x+g_Parameters.volume.hierarchy.y*rootInfo.nodes.y);
    float3 extent=float3(g_Levels[0].nodeExtent.xyz);
    float3 cell=floor((float3(lower)-float3(root.position))/extent)*extent+float3(root.position);
    [branch] if(all(float3(lower+1)<cell+extent)) {
        VDBNode leaf;uint leafIndex;
        bool present=findLeaf(g_Nodes,g_Children,g_Levels,g_Parameters.volume,float3(lower)+.5,leaf,leafIndex);
        [branch] if(!present)return g_Parameters.options.x;
        int3 local=lower-leaf.position;
        [branch] if(all(local>=0)&&all(local+1<g_Levels[0].nodeExtent.xyz)) {
            supported=true;float interior=0;
            [loop] for(uint corner=0;corner<8;++corner) {
                int3 offset=int3(corner&1,(corner>>1)&1,(corner>>2)&1);
                float3 w=lerp(1-fraction,fraction,float3(offset));
                uint address=flatten3(uint3(leaf.atlasPosition+local+offset),g_Parameters.volume.atlasResolutionAndApron.xyz);
                interior+=w.x*w.y*w.z*loadAtlasScalar(address);
            }
            return interior;
        }
    }
    float value=0;
    [loop] for(uint corner=0;corner<8;++corner) {
        int3 offset=int3(corner&1,(corner>>1)&1,(corner>>2)&1);
        float3 w=lerp(1-fraction,fraction,float3(offset));float weight=w.x*w.y*w.z;
        if(weight<=0)continue;
        uint address=0;bool present=voxelAddress(lower+offset,address);
        supported=supported||present;
        value+=weight*(present?loadAtlasScalar(address):g_Parameters.options.x);
    }
    return value;
}
float scalarValue(float3 position) {bool supported;return scalarValue(position,supported);}
float3 transferColor() {
    float coordinate=saturate((g_Parameters.valueRange.z-g_Parameters.valueRange.x)/
        (g_Parameters.valueRange.y-g_Parameters.valueRange.x))*float(g_Parameters.image.z-1);
    uint lo=uint(coordinate);
    return lerp(g_Transfer[lo].xyz,g_Transfer[min(lo+1,g_Parameters.image.z-1)].xyz,coordinate-float(lo));
}
float3 hitColor(float3 position) {
    float3 result=transferColor();
    [branch] if(g_Parameters.options.y!=0) {
        uint address=0;
        [branch] if(voxelAddress(int3(floor(position)),address)) {
            uint packed=g_Color.Load(address*4);
            result=float3(packed&255,(packed>>8)&255,(packed>>16)&255)/255.0;
        }
    }
    return result;
}
bool interval(float3 origin,float3 direction,out float enter,out float leave) {
    float2 clip=rayClipInterval(direction);enter=clip.x;leave=clip.y;
    if(all(direction==0)||!all(isfinite(direction)))return false;
    [loop] for(uint axis=0;axis<3;++axis) {
        float lo=g_Parameters.volume.boundsMin[axis],hi=g_Parameters.volume.boundsMax[axis];
        if(direction[axis]==0){if(origin[axis]<lo||origin[axis]>=hi)return false;}
        else {float a=(lo-origin[axis])/direction[axis],b=(hi-origin[axis])/direction[axis];
            enter=max(enter,min(a,b));leave=min(leave,max(a,b));}
    }
    return enter<leave;
}
bool crosses(float a,float b,bool supportedA,bool supportedB) {
    return (a==0&&supportedA)||(b==0&&supportedB)||(a<0&&b>0)||(a>0&&b<0);
}
[shader("compute")]
[numthreads(8,8,1)]
void renderSurface(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=g_Parameters.image.xy))return;
    uint pixel=id.y*g_Parameters.image.x+id.x;
#if defined(GVDB_SURFACE_RAW)
    // Empty/all-inactive publications have no readable root or atlas allocation.
    [branch] if(g_Parameters.options.z==0) {
        g_Output[pixel]=g_Parameters.background;g_Depth[pixel]=asfloat(0x7f800000u);return;
    }
#endif
    float3 origin=g_Parameters.rayOrigin.xyz;
    float3 direction=g_Parameters.direction00.xyz+(float(id.x)+.5)*g_Parameters.directionDx.xyz+
        (float(id.y)+.5)*g_Parameters.directionDy.xyz;
    float lengthSquared=dot(direction,direction);
    direction=lengthSquared>0&&isfinite(lengthSquared)?direction*rsqrt(lengthSquared):float3(0);
    float4 color=g_Parameters.background;float depth=asfloat(0x7f800000u);
    float enter,leave;
    if(interval(origin,direction,enter,leave)) {
        float previousT=enter;bool previousSupported;
        float previous=scalarValue(origin+direction*enter,previousSupported)-g_Parameters.valueRange.z;
        [loop] for(uint step=0;step<g_Parameters.image.w;++step) {
            float t=min(enter+float(step+1)*g_Parameters.march.x,leave);bool supported;
            float value=scalarValue(origin+direction*t,supported)-g_Parameters.valueRange.z;
            if(crosses(previous,value,previousSupported,supported)) {
                float low=previousT,high=t;
                if(previous==0&&previousSupported)high=low;
                else if(value==0&&supported)low=high;
                else {
                    [loop] for(uint refine=0;refine<10;++refine) {
                        float mid=low+(high-low)*.5;
                        float sample=scalarValue(origin+direction*mid)-g_Parameters.valueRange.z;
                        if(sample==0){low=mid;high=mid;break;}
                        if((previous<0&&sample<0)||(previous>0&&sample>0))low=mid;else high=mid;
                    }
                }
                depth=low+(high-low)*.5;float3 hit=origin+direction*depth;
                float3 gradient=float3(scalarValue(hit+float3(.5,0,0))-scalarValue(hit-float3(.5,0,0)),
                    scalarValue(hit+float3(0,.5,0))-scalarValue(hit-float3(0,.5,0)),
                    scalarValue(hit+float3(0,0,.5))-scalarValue(hit-float3(0,0,.5)));
                float gradientLength=dot(gradient,gradient);
                float3 normal=gradientLength>1e-12&&isfinite(gradientLength)&&all(isfinite(gradient))?
                    gradient*rsqrt(gradientLength):-direction;
                float3 light=g_Parameters.light.xyz-hit;float lightLength=dot(light,light);
                float diffuse=lightLength>0&&isfinite(lightLength)?max(0.0,dot(normal,light*rsqrt(lightLength))):0;
                float illumination=g_Parameters.light.w+(1-g_Parameters.light.w)*diffuse;
                color=float4(hitColor(hit)*illumination,1);
                break;
            }
            if(t>=leave||t<=previousT)break;
            previousT=t;previous=value;previousSupported=supported;
        }
    }
    g_Output[pixel]=color;g_Depth[pixel]=depth;
}
