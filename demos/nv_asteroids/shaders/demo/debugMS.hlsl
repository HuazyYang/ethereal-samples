/*
 * debugMS.hlsl - mesh shader of the asteroid bounding-box debug pipeline.
 *
 * Launched by debugTS.hlsl (one group per asteroid when cbFrame.showBBoxes is set). The last
 * mesh group of the asteroid draws the object bounding box cbObjectInfo.bbox as 8 vertices /
 * 12 triangles: threads 0..7 emit the corners, threads 8..19 emit one triangle each.
 *
 * Output slots: the 2018 shader wrote generic attribute slots of the basic Output layout
 * (SV_Position, POS=1, UV=2, NORMAL=3, TANGENT=4, BITANGENT=5, PREV_WORLD_POS=6):
 *   slot 1  POS        = world position
 *   slot 2  UV         = (1, 0)
 *   slot 3  NORMAL     = (1, 0, 0)
 *   slot 4  TANGENT    = per-PRIMITIVE (lodAlpha, lodIndex, distanceAlpha, lod)
 *   slot 5  BITANGENT  = previous-frame world position (camera-relative translation delta)
 *   slot 6  PREV_WORLD_POS is never written.
 * This port keeps that slot assignment (see meshlet_shaders.NOTES.md, "debugMS").
 *
 * Permutations: DEPTH_PRE_PASS=0 (the only one shipped).
 *
 * Ported from the 2018 NVAPI mesh-shader extension to SM 6.5 (ms_6_5); see
 * meshlet_shaders.NOTES.md for the opcode mapping.
 */

#include "include/meshlet_common.hlsli"

#define BBOX_NUM_VERTICES   8
#define BBOX_NUM_TRIANGLES  12

struct DebugVertexOutput
{
    float4 i_position : SV_POSITION;
    float3 m_pos : POS;
    float2 m_uv : UV;
    centroid float3 m_normal : NORMAL;
    centroid float3 m_bitangent : BITANGENT;    // holds the previous-frame position (original slot 5)
    float3 m_prevPos : PREV_WORLD_POS;          // not written by the original (left 0 here)
};

struct DebugPrimitiveOutput
{
    nointerpolation float4 m_tangent : TANGENT; // original per-primitive slot 4: (lodAlpha, lodIndex, distanceAlpha, lod)
};

static const uint bboxIB[BBOX_NUM_TRIANGLES * 3] =
{
    0, 1, 2,   2, 1, 3,     // -z
    0, 4, 5,   0, 5, 1,     // -y
    1, 5, 7,   1, 7, 3,     // +x
    2, 7, 6,   3, 7, 2,     // +y
    0, 2, 6,   0, 6, 4,     // -x
    4, 6, 5,   5, 6, 7      // +z
};

[outputtopology("triangle")]
[numthreads(MESHLET_GROUP_SIZE, 1, 1)]
void ms_main(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID,
    in payload AsteroidTaskPayload payload,
    out vertices DebugVertexOutput verts[BBOX_NUM_VERTICES],
    out primitives DebugPrimitiveOutput prims[BBOX_NUM_TRIANGLES],
    out indices uint3 tris[BBOX_NUM_TRIANGLES])
{
    uint laneId = groupThreadId.x;
    uint taskIndex = groupId.x;

    bool drawBox = (cbFrame.showBBoxes != 0) && (taskIndex == payload.numMeshTasks - 1);

    // The original emitted nothing (no primitive count) when drawBox is false.
    SetMeshOutputCounts(drawBox ? BBOX_NUM_VERTICES : 0, drawBox ? BBOX_NUM_TRIANGLES : 0);

    if (!drawBox)
        return;

    // DrawAsteroidsBBox(laneId, asteroidIndex, lodIndex, lodInfo) in the original; returns 12.
    uint lodIndex = (taskIndex >= uint(payload.numMeshletsFirstLod)) ? 1 : 0;
    AsteroidInstance instance = instanceInfoBuffer[payload.asteroidIndex];

    if (laneId < BBOX_NUM_VERTICES)
    {
        float4 corner = getBoxCorner(cbObjectInfo.bbox.bboxMin.xyz, cbObjectInfo.bbox.bboxMax.xyz, laneId);
        float3 worldPos = mul(instance.instanceMat, corner) + cbSectorInfo.sectorOffset;
        float3 prevWorldPos = worldPos + cbFrame.preViewTranslationPrevious.xyz - cbFrame.preViewTranslation.xyz;

        float4 viewPos = mul(float4(worldPos, 1.0), cbFrame.matWorldToView);

        DebugVertexOutput o = (DebugVertexOutput)0;
        o.i_position = mul(viewPos, cbFrame.matViewToClip);
        o.m_pos = worldPos;
        o.m_uv = float2(1.0, 0.0);
        o.m_normal = float3(1.0, 0.0, 0.0);
        o.m_bitangent = prevWorldPos;
        verts[laneId] = o;
    }
    else if (laneId < BBOX_NUM_VERTICES + BBOX_NUM_TRIANGLES)
    {
        uint triangleIndex = laneId - BBOX_NUM_VERTICES;

        DebugPrimitiveOutput prim;
        prim.m_tangent = float4(payload.lodAlpha, float(lodIndex), payload.distanceAlpha, payload.lod);
        prims[triangleIndex] = prim;

        tris[triangleIndex] = uint3(bboxIB[triangleIndex * 3 + 0],
                                    bboxIB[triangleIndex * 3 + 1],
                                    bboxIB[triangleIndex * 3 + 2]);
    }
}
