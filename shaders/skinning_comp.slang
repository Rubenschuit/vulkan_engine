// Per-vertex linear blend skinning. One dispatch per skinned mesh instance.
//
// in_vertices / in_skin point at the whole shared mega VBO / mega skin VBO; each
// dispatch reads its mesh's slice starting at input_vertex_offset / input_skin_offset.
// Outputs remain per-instance (out_vertices 48B + out_positions 12B).
//
// Palette is in mesh-entity-local space.

struct SkinPush {
	uint vertex_count;
	uint palette_offset;       // in matrices
	uint input_vertex_offset;
	uint input_skin_offset;
};

[[vk::push_constant]] SkinPush push;

[[vk::binding(0, 0)]] StructuredBuffer<float4> in_vertices;        // 48B vertex stride: read as 3 x float4
[[vk::binding(1, 0)]] StructuredBuffer<uint4> in_skin;             // 16B SkinVertex: 4 uint16 joints + 4 uint16 weights packed into uint4
[[vk::binding(2, 0)]] StructuredBuffer<float4x4> palette;
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> out_vertices;     // 48B vertex stride
[[vk::binding(4, 0)]] RWByteAddressBuffer out_positions;

[numthreads(64, 1, 1)]
void compMain(uint3 tid : SV_DispatchThreadID) {
	uint v = tid.x;
	if (v >= push.vertex_count)
		return;

	uint in_v = push.input_vertex_offset + v;
	uint in_s = push.input_skin_offset + v;

	// Read 48B vertex as three float4:
	float4 chunk0 = in_vertices[in_v * 3 + 0];
	float4 chunk1 = in_vertices[in_v * 3 + 1];
	float4 chunk2 = in_vertices[in_v * 3 + 2];

	float3 pos     = chunk0.xyz;
	float3 normal  = float3(chunk0.w, chunk1.x, chunk1.y);
	float2 uv      = float2(chunk1.z, chunk1.w);
	float4 tangent = chunk2;

	// Decode SkinVertex: 4 uint16 joints
	uint4 packed = in_skin[in_s];
	uint joints[4] = {
		packed.x & 0xFFFFu, (packed.x >> 16) & 0xFFFFu,
		packed.y & 0xFFFFu, (packed.y >> 16) & 0xFFFFu,
	};
	float weights[4] = {
		float(packed.z & 0xFFFFu) / 65535.0f, float((packed.z >> 16) & 0xFFFFu) / 65535.0f,
		float(packed.w & 0xFFFFu) / 65535.0f, float((packed.w >> 16) & 0xFFFFu) / 65535.0f,
	};

	float4x4 skin = weights[0] * palette[push.palette_offset + joints[0]];
	skin += weights[1] * palette[push.palette_offset + joints[1]];
	skin += weights[2] * palette[push.palette_offset + joints[2]];
	skin += weights[3] * palette[push.palette_offset + joints[3]];

	float3x3 skin3 = (float3x3)skin;
	float3x3 cofactor;
	cofactor[0] = cross(skin3[1], skin3[2]);
	cofactor[1] = cross(skin3[2], skin3[0]);
	cofactor[2] = cross(skin3[0], skin3[1]);

	float3 skinned_pos    = mul(skin, float4(pos, 1.0)).xyz;
	float3 skinned_normal = mul(cofactor, normal);
	float3 skinned_tan    = mul(skin3, tangent.xyz);

	out_vertices[v * 3 + 0] = float4(skinned_pos, skinned_normal.x);
	out_vertices[v * 3 + 1] = float4(skinned_normal.y, skinned_normal.z, uv.x, uv.y);
	out_vertices[v * 3 + 2] = float4(skinned_tan, tangent.w);

	out_positions.Store3(v * 12, asuint(skinned_pos));
}