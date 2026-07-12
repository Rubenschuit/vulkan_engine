// Screen-space shadow mask: evaluates CSM shadows once per pixel from the depth buffer.
// Dispatched on the graphics command buffer after the geometry prepass (reads current frame's depth).
// Output is a R8_UNORM image sampled by PBR/simple shaders instead of per-fragment CSM.
// Always reads single-sample depth (resolved from MSAA if active).

#include "ve_common.slangh"

// Set 1: Compute I/O (depth input + shadow mask output)
[vk::binding(0, 1)]
Texture2D<float> depth_texture;      // depth buffer (single-sample resolved)
[vk::binding(1, 1)]
RWTexture2D<float2> shadow_mask_out;  // .x = CSM visibility, .y = view-space depth (RG16F, for bilateral upsample)
[vk::binding(2, 1)]
Texture2D<float4> normal_roughness_texture;  // geometry prepass G-buffer

struct ShadowMaskPush {
	float2 mask_size;   // shadow mask output resolution (may be half-res)
	float2 depth_size;  // depth buffer resolution (always full screen)
};
[[vk::push_constant]] ShadowMaskPush push;

// Reconstruct world position from UV + depth via inverse projection-view
float3 worldFromDepth(float2 uv, float d) {
	float2 ndc = uv * 2.0 - 1.0;
	float4 w = mul(ubo.inverse_projection_view, float4(ndc, d, 1.0));
	return w.xyz / w.w;
}

[numthreads(16, 16, 1)]
void compMain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= uint(push.mask_size.x) || id.y >= uint(push.mask_size.y))
		return;

	// UV in [0,1] from mask pixel coordinate, then map to full-res depth pixel
	float2 uv = (float2(id.xy) + 0.5) / push.mask_size;
	int2 depth_coord = int2(uv * push.depth_size);

	float depth = depth_texture.Load(int3(depth_coord, 0));

	// Sky / far plane: fully lit
	if (depth <= 0.0) {
		shadow_mask_out[id.xy] = float2(1.0, 60000.0); 
		return;
	}

	// Reconstruct world position from depth
	float3 world_pos = worldFromDepth(uv, depth);

	// G-buffer normal
	float3 world_normal = normalize(normal_roughness_texture.Load(int3(depth_coord, 0)).xyz);

	// View-space depth for cascade selection
	float view_z = -mul(ubo.view, float4(world_pos, 1.0)).z;

	// Evaluate CSM shadow
	uint cascade_unused;
	float blend_unused;
	float visibility = calculateCSMShadow(world_pos, view_z, world_normal, cascade_unused, blend_unused);
	shadow_mask_out[id.xy] = float2(visibility, view_z);
}
