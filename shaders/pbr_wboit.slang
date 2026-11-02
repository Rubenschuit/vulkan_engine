// WBOIT (Weighted Blended Order-Independent Transparency) geometry pass.
// Uses the same PBR lighting as pbr_shader.slang but outputs to 2 MRT targets:
//   RT0 (RGBA16F): accumulated weighted premultiplied color
//   RT1 (R16F): revealage (product of 1-alpha via multiplicative blending)

#include "pbr_common.slangh"

struct WboitOutput {
	float4 accum : SV_Target0;
	float revealage : SV_Target1;
};

[shader("fragment")]
WboitOutput fragMain(VertexOutput in_vert) {
	float4 c = computePbrLighting(in_vert);

	float alpha = c.a;
	float3 premultiplied = c.rgb * alpha;

	// McGuire & Bavoil 2013 depth-based weight function
	float z = in_vert.pos.z; // NDC depth [0, 1]
	float dz5 = z / 5.0;
	float dz200 = z / 200.0;
	float w = alpha * max(1e-2, min(3e3,
		10.0 / (1e-5 + dz5 * dz5 + dz200 * dz200 * dz200 * dz200 * dz200 * dz200)));

	WboitOutput out;
	out.accum = float4(premultiplied * w, alpha * w);
	out.revealage = alpha;
	return out;
}