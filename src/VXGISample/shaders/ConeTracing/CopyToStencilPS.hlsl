#include "../Common/ShaderCommon.hlsli"
#include "ConeTracingConstants.hlsli"

Texture2D t_RefinementControl : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0, 0);
void main(VxgiFullScreenQuadOutput quadIn, in float4 gl_FragCoord: SV_Position)
{
if (t_RefinementControl[int2(gl_FragCoord.xy)].x == 0)
discard;
}
