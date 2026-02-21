// Screen-space shadow mask: evaluates CSM shadows once per pixel from the depth buffer.
// Dispatched on the compute queue with one-frame latency (reads previous frame's depth).
// Output is a R8_UNORM image sampled by PBR/simple shaders instead of per-fragment CSM.
// Always reads single-sample depth (resolved from MSAA if active).

#include "ve_common.slangh"

// Set 1: Compute I/O (depth input + shadow mask output)
[vk::binding(0, 1)]
Texture2D<float> depth_texture;      // depth buffer (single-sample resolved)
[vk::binding(1, 1)]
RWTexture2D<float> shadow_mask_out;  // R8 output

struct ShadowMaskPush {
	float2 mask_size;   // shadow mask output resolution (may be half-res)
	float2 depth_size;  // depth buffer resolution (always full screen)
};
[[vk::push_constant]] ShadowMaskPush push;

[numthreads(16, 16, 1)]
void compMain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= uint(push.mask_size.x) || id.y >= uint(push.mask_size.y))
		return;

	// UV in [0,1] from mask pixel coordinate, then map to full-res depth pixel
	float2 uv = (float2(id.xy) + 0.5) / push.mask_size;
	int2 depth_coord = int2(uv * push.depth_size);

	float depth = depth_texture.Load(int3(depth_coord, 0));

	// Sky / far plane: fully lit
	if (depth >= 1.0) {
		shadow_mask_out[id.xy] = 1.0;
		return;
	}

	// Reconstruct world position from depth
	float2 ndc = uv * 2.0 - 1.0;
	float4 world = mul(ubo.inverse_projection_view, float4(ndc, depth, 1.0));
	world /= world.w;

	// View-space depth for cascade selection
	float view_z = -mul(ubo.view, float4(world.xyz, 1.0)).z;

	// Evaluate CSM shadow
	uint cascade_unused;
	float blend_unused;
	shadow_mask_out[id.xy] = calculateCSMShadow(world.xyz, view_z, cascade_unused, blend_unused);
}
