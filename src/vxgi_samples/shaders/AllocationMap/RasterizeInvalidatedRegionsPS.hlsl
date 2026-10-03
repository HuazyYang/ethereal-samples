#include "../Common/ShaderCommon.hlsli"
RWTexture3D<uint> u_InvalidationBitmap: register(u1);
void main(in float4 gl_FragCoord: SV_Position, in uint4 zbits: Z_BITS)
{
    [unroll]
        for (uint z = 0; z < 4; z++)
    {
        if (zbits[z] != 0)
        {
            InterlockedOr(u_InvalidationBitmap[int3(gl_FragCoord.xy, z)], zbits[z]);
        }
    }
}