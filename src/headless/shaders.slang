/*
* Copyright (c) 2023, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

StructuredBuffer<uint> t_Input : register(t0);
RWStructuredBuffer<uint> u_Output : register(u0);

groupshared uint s_ReductionData[256];

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint threadIdx : SV_GroupIndex)
{
    s_ReductionData[threadIdx] = t_Input[threadIdx];
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128; stride != 0; stride >>= 1)
    {
        if (threadIdx < stride)
            s_ReductionData[threadIdx] += s_ReductionData[threadIdx + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (threadIdx == 0)
        u_Output[0] = s_ReductionData[0];
}
