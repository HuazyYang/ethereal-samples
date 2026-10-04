/*
 * basicTS.hlsl - task (amplification) shader of the generic (non-asteroid) meshlet pipeline,
 * used for regular meshlet objects such as the ship.
 *
 * No culling: launches one mesh group (basicMS.hlsl) per meshlet of the draw,
 * i.e. cbMeshletInfo.numMeshlets groups starting at cbMeshletInfo.firstMeshlet.
 *
 * Permutations: DEPTH_PRE_PASS={0,1}, _MESHLETS_HI_Z=0. Neither define changes the logic; in the
 * 2018 binaries DEPTH_PRE_PASS only selected the dummy vertex-shader output signature.
 *
 * Ported from the 2018 NVAPI mesh-shader extension to SM 6.5 (as_6_5); see
 * meshlet_shaders.NOTES.md for the opcode mapping.
 */

#include "include/meshlet_common.hlsli"

groupshared BasicTaskPayload s_Payload;

[numthreads(MESHLET_GROUP_SIZE, 1, 1)]
void ts_main()
{
    // The original issued the task count from lane 0 only; DispatchMesh is a group-wide call.
    // Note: SM 6.5 limits each DispatchMesh dimension to 65535 groups.
    DispatchMesh(cbMeshletInfo.numMeshlets, 1, 1, s_Payload);
}
