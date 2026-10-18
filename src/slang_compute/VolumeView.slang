// Portable section/voxel rendering and projected topology lines.
// SPDX-License-Identifier: Apache-2.0
#include "GVDBRaycastCommon.slang"
struct ViewSettings {
    uint4 rectangle;
    uint4 lineGrid;
    float4 section;
    uint4 flags;
};
ConstantBuffer<ViewSettings> g_View : register(b1);
#if !defined(GVDB_OWNED_VIEW)
struct Line {float4 points;float4 color;};
StructuredBuffer<Line> g_Lines : register(t5);
StructuredBuffer<uint2> g_TileRanges : register(t6);
StructuredBuffer<uint> g_LineIndices : register(t7);
#endif
bool nearestValue(float3 position,out float value) {
    VDBNode leaf;uint index;value=0;
    if(!findLeaf(g_Nodes,g_Children,g_Levels,g_Parameters.volume,position,leaf,index))return false;
    value=atlasValue(leaf.atlasPosition+int3(floor(position-float3(leaf.position))));
    return true;
}
float4 shaded(float3 hit,float3 direction,float density,bool voxel,float3 voxelMinimum=float3(0)) {
    float3 normal=-direction;
    if(voxel) {
        float3 relative=saturate(hit-voxelMinimum),faces=min(relative,1-relative);
        uint axis=faces.x<faces.y?(faces.x<faces.z?0u:2u):(faces.y<faces.z?1u:2u);
        float sign=relative[axis]<.5?-1:1;
        normal=axis==0?float3(sign,0,0):axis==1?float3(0,sign,0):float3(0,0,sign);
    } else {
        float3 gradient=float3(sampleVolume(hit+float3(.5,0,0))-sampleVolume(hit-float3(.5,0,0)),
            sampleVolume(hit+float3(0,.5,0))-sampleVolume(hit-float3(0,.5,0)),
            sampleVolume(hit+float3(0,0,.5))-sampleVolume(hit-float3(0,0,.5)));
        if(dot(gradient,gradient)>1e-12)normal=normalize(-gradient);
    }
    float3 toLight=g_Parameters.light.xyz-hit;
    float diffuse=dot(toLight,toLight)>1e-12?max(0.,dot(normal,normalize(toLight))):0;
    return float4(transferValue(density).xyz*(g_Parameters.light.w+(1-g_Parameters.light.w)*diffuse),1);
}
float nextVoxelBoundary(float origin,float direction,int cell) {
    if(direction>0)return (float(cell)+1-origin)/direction;
    if(direction<0)return (float(cell)-origin)/direction;
    return asfloat(0x7f800000u);
}
[shader("compute")][numthreads(8,8,1)]
void renderVoxel(uint3 id:SV_DispatchThreadID) {
    [branch] if(any(id.xy>=g_Parameters.image.xy))return;
    uint pixel=id.y*g_Parameters.image.x+id.x;
#if defined(GVDB_OWNED_VIEW)
    [branch] if(g_View.flags.x==0) {
        g_Output[pixel]=g_Parameters.background;g_Depth[pixel]=asfloat(0x7f800000u);return;
    }
#endif
    float3 origin=g_Parameters.rayOrigin.xyz,direction=pixelDirection(id.xy);
    float enter,leave,depth=asfloat(0x7f800000u);float4 color=g_Parameters.background;
    if(intersectVolume(origin,direction,enter,leave)) {
        float t=enter;
        float3 position=origin+direction*t;
        int3 cell=int3(floor(position));
        // At an exact boundary a negative ray enters the cell on its lower side.
        // Advance integer cells explicitly: a fixed epsilon stops advancing once
        // floating-point ray distances become larger than its precision.
        cell-=int3(direction.x<0&&position.x==float(cell.x)?1:0,
                   direction.y<0&&position.y==float(cell.y)?1:0,
                   direction.z<0&&position.z==float(cell.z)?1:0);
        [loop] for(uint iteration=0;iteration<g_Parameters.image.w&&t<leave;++iteration) {
            float value;
            if(nearestValue(float3(cell)+.5,value)&&value>=g_Parameters.valueRange.z) {
                depth=t;color=shaded(origin+direction*t,direction,value,true,float3(cell));break;
            }
            float3 boundaries=float3(nextVoxelBoundary(origin.x,direction.x,cell.x),
                nextVoxelBoundary(origin.y,direction.y,cell.y),nextVoxelBoundary(origin.z,direction.z,cell.z));
            float next=min(boundaries.x,min(boundaries.y,boundaries.z));
            if(next>=leave)break;
            cell+=int3(boundaries.x<=next?(direction.x>0?1:-1):0,
                       boundaries.y<=next?(direction.y>0?1:-1):0,
                       boundaries.z<=next?(direction.z>0?1:-1):0);
            t=max(t,next);
        }
    }
    g_Output[pixel]=color;g_Depth[pixel]=depth;
}
[shader("compute")][numthreads(8,8,1)]
void renderSection(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=g_Parameters.image.xy))return;
    uint pixel=id.y*g_Parameters.image.x+id.x;
#if defined(GVDB_OWNED_VIEW)
    [branch] if(g_View.flags.x==0) {
        g_Output[pixel]=g_Parameters.background;g_Depth[pixel]=asfloat(0x7f800000u);return;
    }
#endif
    float3 origin=g_Parameters.rayOrigin.xyz,direction=pixelDirection(id.xy);
    float planeT=direction.y!=0?(g_View.section.y-origin.y)/direction.y:-1;
    float enter,leave,depth=asfloat(0x7f800000u);float4 base=g_Parameters.background,section=float4(0);
    float2 clip=rayClipInterval(direction);
    if(planeT>0&&planeT>=clip.x&&planeT<=clip.y) {
        float3 point=origin+direction*planeT;VDBNode leaf;uint index;
        if(findLeaf(g_Nodes,g_Children,g_Levels,g_Parameters.volume,point,leaf,index)) {
            section=transferValue(sampleVolume(point));section.w=saturate(section.w);
            if(section.w>0)depth=planeT;
        }
    }
    if(intersectVolume(origin,direction,enter,leave)) {
        float start=max(enter,max(0.,planeT));
        [loop] for(uint iteration=0;iteration<g_Parameters.image.w;++iteration) {
            float t=start+float(iteration)*g_Parameters.march.x;if(t>=leave)break;
            float3 point=origin+direction*t;float value=sampleVolume(point);
            if(value>=g_Parameters.valueRange.z) {
                base=shaded(point,direction,g_Parameters.valueRange.z,false);
                if(isinf(depth))depth=t;
                break;
            }
        }
    }
    g_Output[pixel]=float4(lerp(base.xyz,section.xyz,section.w),1);g_Depth[pixel]=depth;
}
[shader("compute")][numthreads(8,8,1)]
void renderSection2D(uint3 id:SV_DispatchThreadID) {
    uint2 extent=g_View.rectangle.zw;
    if(any(id.xy>=extent))return;
    uint2 pixel=id.xy+g_View.rectangle.xy;
    if(any(pixel>=g_Parameters.image.xy))return;
#if defined(GVDB_OWNED_VIEW)
    [branch] if(g_View.flags.x==0) {
        g_Output[pixel.y*g_Parameters.image.x+pixel.x]=g_Parameters.background;
        g_Depth[pixel.y*g_Parameters.image.x+pixel.x]=asfloat(0x7f800000u);return;
    }
#endif
    float2 uv=(float2(id.xy)+.5)/float2(extent);
    float3 position=float3(lerp(g_Parameters.volume.boundsMin.x,g_Parameters.volume.boundsMax.x,uv.x),
        g_View.section.y,lerp(g_Parameters.volume.boundsMin.z,g_Parameters.volume.boundsMax.z,uv.y));
    float4 color=transferValue(sampleVolume(position));
    g_Output[pixel.y*g_Parameters.image.x+pixel.x]=float4(color.xyz*saturate(color.w),1);
    g_Depth[pixel.y*g_Parameters.image.x+pixel.x]=0;
}
#if !defined(GVDB_OWNED_VIEW)
[shader("compute")][numthreads(8,8,1)]
void overlayTopology(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=g_Parameters.image.xy))return;
    uint2 tile=id.xy/g_View.lineGrid.x;
    uint2 range=g_TileRanges[tile.y*g_View.lineGrid.y+tile.x];
    float2 pixel=float2(id.xy)+.5;
    bool found=false;float4 color=g_Output[id.y*g_Parameters.image.x+id.x];
    [loop] for(uint i=0;i<range.y;++i) {
        Line line=g_Lines[g_LineIndices[range.x+i]];
        float2 a=line.points.xy,b=line.points.zw,d=b-a;
        float t=dot(d,d)>0?saturate(dot(pixel-a,d)/dot(d,d)):0;
        if(dot(pixel-a-t*d,pixel-a-t*d)<=.7*.7) {
            // Match source-alpha blending in draw order, including overlapping
            // translucent rays. Zero-alpha lines leave the scene unchanged.
            float alpha=saturate(line.color.w);
            color=float4(lerp(color.xyz,line.color.xyz,alpha),alpha+color.w*(1-alpha));
            found=true;
        }
    }
    // Keep the UAV side effect outside the dynamic loop for FXC.
    [branch] if(!found)return;
    g_Output[id.y*g_Parameters.image.x+id.x]=color;
}

#endif
