// SPDX-License-Identifier: MIT
RWTexture2D<uint> sharedTexture : register(u0);
RWStructuredBuffer<uint> sharedBuffer : register(u1);
[shader("compute")]
[numthreads(8,4,1)]
void updateShared(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy>=uint2(8,4))) return;
    uint index=id.y*8+id.x;
    sharedTexture[id.xy] += sharedBuffer[index];
    sharedBuffer[index] += 1;
}
