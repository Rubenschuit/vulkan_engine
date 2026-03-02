#include "pbr_common.slangh"

[shader("fragment")]
float4 fragMain(VertexOutput in_vert) : SV_Target {
	return computePbrLighting(in_vert);
}
