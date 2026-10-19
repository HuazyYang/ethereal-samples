// SPDX-License-Identifier: Apache-2.0
struct SubcellParameters { uint4 info; float4 translation; };
ConstantBuffer<SubcellParameters> g_Parameters:register(b0);
ByteAddressBuffer g_Indices:register(t0);
ByteAddressBuffer g_Positions:register(t1);
ByteAddressBuffer g_Velocities:register(t2);
ByteAddressBuffer g_Colors:register(t3);
RWByteAddressBuffer g_OutputPositions:register(u0);
RWByteAddressBuffer g_OutputVelocities:register(u1);
RWByteAddressBuffer g_OutputColors:register(u2);
[shader("compute")][numthreads(256,1,1)]
void expandSubcellPoints(uint3 id:SV_DispatchThreadID) {
    uint i=id.x+id.y*(65535u*256u);if(i>=g_Parameters.info.x)return;
    uint source=g_Indices.Load(i*4),ps=g_Parameters.info.y,vs=g_Parameters.info.z;
    uint3 bits=g_Positions.Load3(source*ps);
    [unroll] for(uint axis=0;axis<3;++axis)
        if(g_Parameters.translation[axis]!=0)bits[axis]=asuint(asfloat(bits[axis])+g_Parameters.translation[axis]);
    g_OutputPositions.Store3(i*ps,bits);
    if(ps==16)g_OutputPositions.Store(i*ps+12,g_Positions.Load(source*ps+12));
    if((g_Parameters.info.w&2)!=0) {
        g_OutputVelocities.Store3(i*vs,g_Velocities.Load3(source*vs));
        if(vs==16)g_OutputVelocities.Store(i*vs+12,g_Velocities.Load(source*vs+12));
    }
    if((g_Parameters.info.w&4)!=0)g_OutputColors.Store(i*4,g_Colors.Load(source*4));
}
