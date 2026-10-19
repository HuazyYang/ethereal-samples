// Retained GVDB getTricubic equations, with explicit linear atlas fetches.
// SPDX-License-Identifier: Apache-2.0
float tricubicSample(float3 local,float3 offset) {
    // Each retained integer-position linear fetch averages its eight corners.
    // Combine overlapping quadratic taps into a 4x4x4 separable stencil.
    // A loop avoids FXC's enormous inlined 27x8 load/branch expression trees.
    int3 base=int3(floor(local+offset))-2;
    float3 t=frac(local)*.5+.25,a=1-t;
    float3 w0=.5*a*a,w1=.5*(a*a+2*a*t),w2=.5*(2*a*t+t*t),w3=.5*t*t;
    float value=0;
    [loop] for(uint i=0;i<64;++i){
        uint3 cell=uint3(i&3,(i>>2)&3,i>>4);
        float3 w=float3(cell.x==0?w0.x:cell.x==1?w1.x:cell.x==2?w2.x:w3.x,
            cell.y==0?w0.y:cell.y==1?w1.y:cell.y==2?w2.y:w3.y,
            cell.z==0?w0.z:cell.z==1?w1.z:cell.z==2?w2.z:w3.z);
        value+=w.x*w.y*w.z*texel(base+int3(cell));
    }
    return value;
}
TraceResult traceTricubic(float3 origin,float3 direction) {
    TraceResult result=(TraceResult)0;result.hit=float4(1e10,1e10,1e10,0);
    if(!finite3(origin)||!finite3(direction)||all(direction==0))return result;
    float enter,leave;if(!rayBox(origin,direction,enter,leave))return result;
    enter=max(enter,g_Parameters.march.y);leave=min(leave,g_Parameters.march.z);
    if(enter>=leave)return result;
    float width=float(g_Levels[0].nodeExtent.x),step=g_Parameters.sampling.x;
    LevelInfo root=g_Levels[g_Parameters.volume.hierarchy.x];
    float3 grid=float3(loadNode(g_Nodes,root.nodes.x+g_Parameters.volume.hierarchy.y*root.nodes.y).position);
    int3 cell=enteredCell(origin,direction,enter,grid,width);float t=enter;uint corrections=0;
    [loop] for(uint visit=0;visit<g_Parameters.counts.z;) {
        if(t>=leave)return result;
        float3 times=cellExitTimes(cell,grid,width,origin,direction);
        float exitT=min(leave,min(times.x,min(times.y,times.z)));
        if(exitT<=t){if(++corrections>3){result.hit.w=-1;return result;}advanceCell(cell,times,exitT,direction);continue;}
        ++visit;VDBNode leaf;uint index;
        if(findLeaf(g_Nodes,g_Children,g_Levels,g_Parameters.volume,grid+float3(cell)*width+width*.5,leaf,index)) {
            float sampleT=leafSamplingEntry(leaf,t,enter,origin,direction);
            uint iteration=0;
            [loop] for(;iteration<g_Parameters.counts.y;++iteration) {
                if(sampleT>=exitT)break;
                float3 local=origin+sampleT*direction-float3(leaf.position),offset=float3(leaf.atlasPosition);
                float value=tricubicSample(local,offset);
                if(value>=g_Parameters.sampling.y) {
                    float backstep=min(g_Parameters.sampling.w,max(0.0,sampleT-enter));
                    float previous=tricubicSample(local-backstep*direction,offset);
                    float denominator=value-previous;
                    // Refine only a finite bracket. Legacy unbracketed extrapolation can
                    // leave the atlas coordinate domain or divide by zero.
                    float correction=denominator>0&&((asuint(denominator)&0x7fffffffu)<0x7f800000u)&&previous<g_Parameters.sampling.y?
                        saturate((value-g_Parameters.sampling.y)/denominator):0;
                    if((asuint(correction)&0x7fffffffu)>=0x7f800000u)correction=0;
                    float3 refined=local-correction*backstep*direction;
                    if(finite3(refined))local=refined;
                    float3 n=float3(tricubicSample(local-float3(.5,0,0),offset)-tricubicSample(local+float3(.5,0,0),offset),
                        tricubicSample(local-float3(0,.5,0),offset)-tricubicSample(local+float3(0,.5,0),offset),
                        tricubicSample(local-float3(0,0,.5),offset)-tricubicSample(local+float3(0,0,.5),offset));
                    float norm=dot(n,n);
                    n=norm>1e-12&&((asuint(norm)&0x7fffffffu)<0x7f800000u)&&finite3(n)?n*rsqrt(norm):-normalize(direction);
                    result.hit=float4(local+float3(leaf.position),1);result.normal=float4(n,0);result.atlas=local+offset;return result;
                }
                float next=sampleT+step;if(next<=sampleT){result.hit.w=-1;return result;}sampleT=next;
            }
            if(iteration==g_Parameters.counts.y&&sampleT<exitT){result.hit.w=-1;return result;}
        }
        advanceCell(cell,times,exitT,direction);t=exitT;
    }
    if(t<leave)result.hit.w=-1;return result;
}
