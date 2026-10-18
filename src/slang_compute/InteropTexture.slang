// SPDX-License-Identifier: MIT
RWTexture2D<uint> sharedTexture : register(u0);
RWStructuredBuffer<uint> sharedBuffer : register(u1);
[shader("compute")]
[numthreads(8,4,1)]
void updateShared(uint3 id : SV_DispatchThreadID)
{
    uint width,height; sharedTexture.GetDimensions(width,height);
    if(any(id.xy>=uint2(width,height))) return;
    uint index=id.y*width+id.x;
    sharedTexture[id.xy] += sharedBuffer[index];
    sharedBuffer[index] += 1;
}
