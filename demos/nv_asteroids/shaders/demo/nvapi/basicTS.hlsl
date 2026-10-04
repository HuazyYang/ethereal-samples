/*
 * nvapi/basicTS.hlsl - the ORIGINAL 2018 NVAPI form of the basic-object task shader (vs_6_0, entry ts_main).
 * Thread 0 launches cbMeshletInfo.numMeshlets mesh groups (basicMS); no payload.
 * Reproduces basicTS_ts_main (DEPTH_PRE_PASS, _MESHLETS_HI_Z=0).
 */

#define NV_SHADER_EXTN_SLOT u7
#include "nv_meshlet_extns.hlsli"
#include "../include/meshlet_common.hlsli"

#define NV_OUTPUT_ASTEROIDS 0
#include "nv_outputs.hlsli"

VertexOutput ts_main()
{
    if (__NvMeshGetThreadId() == 0)
        __NvMeshSetTaskCount(cbMeshletInfo.numMeshlets);

    return DummyOutput();
}
