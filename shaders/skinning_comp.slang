// Per-vertex linear blend skinning for all skinned entities in a single dispatch.
//
// One workgroup processes 64 vertices of one skinning slot. The CPU builds a
// table `wg_info[]` with one entry per workgroup.
//
// Inputs live in the static / sparse-skin regions of the mega VBO:
//   mega_vbo[input_vertex_offset + v]  — bind-pose VeMesh::Vertex (48 B)
//   mega_skin[input_skin_offset + v]   — SkinVertex (16 B)
//   palette[palette_offset + joint]    — joint matrices (mesh-entity-local)
//
// Outputs are written into the dynamic region of the same mega VBOs at
// output_vertex_offset. Slots are disjoint by construction.

struct SkinWorkgroupInfo {
	uint vertex_count;          // total vertices in this slot
	uint palette_offset;        // in matrices
	uint input_vertex_offset;   // in vertices, into the mega VBO
	uint input_skin_offset;     // in skin vertices, into the mega skin VBO
	uint output_vertex_offset;  // in vertices, into the mega VBO dynamic region
	uint local_vertex_base;     // first vertex index this workgroup handles (0, 64, 128, ...)
	uint _pad0;
	uint _pad1;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<float4> mega_vbo;          // 48B vertex stride: read bind-pose, write skinned output
[[vk::binding(1, 0)]] StructuredBuffer<uint4> mega_skin;            // 16B SkinVertex: 4 uint16 joints + 4 uint16 weights packed into uint4
[[vk::binding(2, 0)]] StructuredBuffer<float4x4> palette;
[[vk::binding(3, 0)]] RWByteAddressBuffer mega_shadow;              // 12B vec3 stride: write position-only output
[[vk::binding(4, 0)]] StructuredBuffer<SkinWorkgroupInfo> wg_info;

[numthreads(64, 1, 1)]
void compMain(uint3 group_id : SV_GroupID, uint3 group_thread_id : SV_GroupThreadID) {
	SkinWorkgroupInfo info = wg_info[group_id.x];
	uint v = info.local_vertex_base + group_thread_id.x;
	if (v >= info.vertex_count)
		return;

	uint in_v = info.input_vertex_offset + v;
	uint in_s = info.input_skin_offset + v;
	uint out_v = info.output_vertex_offset + v;

	// Read 48B vertex as three float4:
	float4 chunk0 = mega_vbo[in_v * 3 + 0];
	float4 chunk1 = mega_vbo[in_v * 3 + 1];
	float4 chunk2 = mega_vbo[in_v * 3 + 2];

	float3 pos     = chunk0.xyz;
	float3 normal  = float3(chunk0.w, chunk1.x, chunk1.y);
	float2 uv      = float2(chunk1.z, chunk1.w);
	float4 tangent = chunk2;

	uint4 packed = mega_skin[in_s];
	uint joints[4] = {
		packed.x & 0xFFFFu, (packed.x >> 16) & 0xFFFFu,
		packed.y & 0xFFFFu, (packed.y >> 16) & 0xFFFFu,
	};
	float weights[4] = {
		float(packed.z & 0xFFFFu) / 65535.0f, float((packed.z >> 16) & 0xFFFFu) / 65535.0f,
		float(packed.w & 0xFFFFu) / 65535.0f, float((packed.w >> 16) & 0xFFFFu) / 65535.0f,
	};

	float4x4 skin = weights[0] * palette[info.palette_offset + joints[0]];
	skin += weights[1] * palette[info.palette_offset + joints[1]];
	skin += weights[2] * palette[info.palette_offset + joints[2]];
	skin += weights[3] * palette[info.palette_offset + joints[3]];

	float3x3 skin3 = (float3x3)skin;
	float3x3 cofactor;
	cofactor[0] = cross(skin3[1], skin3[2]);
	cofactor[1] = cross(skin3[2], skin3[0]);
	cofactor[2] = cross(skin3[0], skin3[1]);

	float3 skinned_pos    = mul(skin, float4(pos, 1.0)).xyz;
	float3 skinned_normal = mul(cofactor, normal);
	float3 skinned_tan    = mul(skin3, tangent.xyz);

	mega_vbo[out_v * 3 + 0] = float4(skinned_pos, skinned_normal.x);
	mega_vbo[out_v * 3 + 1] = float4(skinned_normal.y, skinned_normal.z, uv.x, uv.y);
	mega_vbo[out_v * 3 + 2] = float4(skinned_tan, tangent.w);

	mega_shadow.Store3(out_v * 12, asuint(skinned_pos));
}