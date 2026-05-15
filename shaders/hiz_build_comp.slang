// Hierarchical-Z pyramid build compute shader.
// 32x32 workgroups load source data into shared memory, then perform up to 5
// ping-pong 2x2 MIN reductions, writing each mip level along the way.
// Reverse-Z (near=1, far=0): MIN keeps the farthest depth per cell.
//
// Pass 1 (write_source=1): loads depth buffer 1:1, writes mip 0 + 5 reductions = 6 mips
// Pass 2+ (write_source=0): loads previous pass's last mip, 5 reductions = 5 mips
//
// Dispatch: (ceil(dst_w / 32), ceil(dst_h / 32), 1)

[[vk::binding(0, 0)]] Texture2D<float>   src_image;
[[vk::binding(1, 0)]] SamplerState       point_sampler;
[[vk::binding(2, 0)]] RWTexture2D<float> dst_mip0;
[[vk::binding(3, 0)]] RWTexture2D<float> dst_mip1;
[[vk::binding(4, 0)]] RWTexture2D<float> dst_mip2;
[[vk::binding(5, 0)]] RWTexture2D<float> dst_mip3;
[[vk::binding(6, 0)]] RWTexture2D<float> dst_mip4;
[[vk::binding(7, 0)]] RWTexture2D<float> dst_mip5;

struct HizPushConstants {
	uint2  dst_size;       // padded (POT) output extent
	uint2  src_size;       // valid source extent: threads past it load 1.0 (MIN identity)
	float2 src_texel_size;
	uint   mip_count;
	uint   write_source;
};

[[vk::push_constant]] HizPushConstants pc;

groupshared float lds_a[32][32];
groupshared float lds_b[32][32];

void storeMip(uint idx, uint2 pos, float val) {
	switch (idx) {
		case 0: dst_mip0[pos] = val; break;
		case 1: dst_mip1[pos] = val; break;
		case 2: dst_mip2[pos] = val; break;
		case 3: dst_mip3[pos] = val; break;
		case 4: dst_mip4[pos] = val; break;
		case 5: dst_mip5[pos] = val; break;
	}
}

[numthreads(32, 32, 1)]
void compMain(uint3 tid : SV_DispatchThreadID,
              uint3 lid : SV_GroupThreadID,
              uint3 gid : SV_GroupID) {
	uint lx = lid.x;
	uint ly = lid.y;

	// Load one texel from source into LDS (OOB = 1.0 = near plane, won't affect MIN)
	float depth = 1.0;
	if (tid.x < pc.src_size.x && tid.y < pc.src_size.y) {
		float2 uv = (float2(tid.xy) + 0.5) * pc.src_texel_size;
		depth = src_image.SampleLevel(point_sampler, uv, 0);
	}
	lds_a[ly][lx] = depth;

	// Optionally write loaded data as first output (pass 1 = mip 0 copy)
	uint out = 0;
	if (pc.write_source != 0) {
		if (tid.x < pc.dst_size.x && tid.y < pc.dst_size.y)
			storeMip(0, tid.xy, depth);
		out = 1;
	}

	if (out >= pc.mip_count)
		return;

	GroupMemoryBarrierWithGroupSync();

	// Reduction 1: 32x32 -> 16x16 (lds_a -> lds_b)
	if (lx < 16 && ly < 16) {
		uint2 s = uint2(lx * 2, ly * 2);
		float m = min(min(lds_a[s.y][s.x], lds_a[s.y][s.x + 1]),
		              min(lds_a[s.y + 1][s.x], lds_a[s.y + 1][s.x + 1]));
		lds_b[ly][lx] = m;

		uint2 pos = gid.xy * 16 + uint2(lx, ly);
		uint2 sz = max(uint2(1, 1), pc.dst_size >> 1);
		if (pos.x < sz.x && pos.y < sz.y)
			storeMip(out, pos, m);
	}
	out++;
	if (out >= pc.mip_count)
		return;

	GroupMemoryBarrierWithGroupSync();

	// Reduction 2: 16x16 -> 8x8 (lds_b -> lds_a)
	if (lx < 8 && ly < 8) {
		uint2 s = uint2(lx * 2, ly * 2);
		float m = min(min(lds_b[s.y][s.x], lds_b[s.y][s.x + 1]),
		              min(lds_b[s.y + 1][s.x], lds_b[s.y + 1][s.x + 1]));
		lds_a[ly][lx] = m;

		uint2 pos = gid.xy * 8 + uint2(lx, ly);
		uint2 sz = max(uint2(1, 1), pc.dst_size >> 2);
		if (pos.x < sz.x && pos.y < sz.y)
			storeMip(out, pos, m);
	}
	out++;
	if (out >= pc.mip_count)
		return;

	GroupMemoryBarrierWithGroupSync();

	// Reduction 3: 8x8 -> 4x4 (lds_a -> lds_b)
	if (lx < 4 && ly < 4) {
		uint2 s = uint2(lx * 2, ly * 2);
		float m = min(min(lds_a[s.y][s.x], lds_a[s.y][s.x + 1]),
		              min(lds_a[s.y + 1][s.x], lds_a[s.y + 1][s.x + 1]));
		lds_b[ly][lx] = m;

		uint2 pos = gid.xy * 4 + uint2(lx, ly);
		uint2 sz = max(uint2(1, 1), pc.dst_size >> 3);
		if (pos.x < sz.x && pos.y < sz.y)
			storeMip(out, pos, m);
	}
	out++;
	if (out >= pc.mip_count)
		return;

	GroupMemoryBarrierWithGroupSync();

	// Reduction 4: 4x4 -> 2x2 (lds_b -> lds_a)
	if (lx < 2 && ly < 2) {
		uint2 s = uint2(lx * 2, ly * 2);
		float m = min(min(lds_b[s.y][s.x], lds_b[s.y][s.x + 1]),
		              min(lds_b[s.y + 1][s.x], lds_b[s.y + 1][s.x + 1]));
		lds_a[ly][lx] = m;

		uint2 pos = gid.xy * 2 + uint2(lx, ly);
		uint2 sz = max(uint2(1, 1), pc.dst_size >> 4);
		if (pos.x < sz.x && pos.y < sz.y)
			storeMip(out, pos, m);
	}
	out++;
	if (out >= pc.mip_count)
		return;

	GroupMemoryBarrierWithGroupSync();

	// Reduction 5: 2x2 -> 1x1 (lds_a)
	if (lx == 0 && ly == 0) {
		float m = min(min(lds_a[0][0], lds_a[0][1]),
		              min(lds_a[1][0], lds_a[1][1]));

		uint2 pos = gid.xy;
		uint2 sz = max(uint2(1, 1), pc.dst_size >> 5);
		if (pos.x < sz.x && pos.y < sz.y)
			storeMip(out, pos, m);
	}
}