// SPDX-License-Identifier: Apache-2.0
cbuffer Parameters:register(b0){uint4 work;float4 translation;};
ByteAddressBuffer source:register(t0);
RWByteAddressBuffer planes:register(u0);
[shader("compute")]
[numthreads(256,1,1)]
void splitPointCoordinates(uint3 group:SV_GroupID,uint local:SV_GroupIndex){
    uint i=(group.y*65535u+group.x)*256u+local;
    if(i>=work.x)return;
    uint3 bits=source.Load3(i*work.y);
    [unroll]for(uint axis=0;axis<3;++axis){
        uint value=bits[axis];
        if(translation[axis]!=0)value=asuint(asfloat(value)+translation[axis]);
        planes.Store((axis*work.x+i)*4,value);
    }
}
